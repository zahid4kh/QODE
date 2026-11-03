#include "McpManager.h"

#include "mcp/McpClient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace mcp {

namespace {
constexpr int kCallTimeoutMs = 300000; // a render or a big query can take a while; Stop ends it sooner
constexpr int kMaxTextChars = 20000;   // every result is sent again with each later request
constexpr int kMaxImageBase64 = 5 * 1024 * 1024; // Claude's limit per image
constexpr int kMaxInstructionChars = 2000;

QString clean(const QString &s)
{
    QString out = s;
    out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    return out;
}

QString qualifiedName(const QString &server, const QString &tool)
{
    QString name = QStringLiteral("mcp__%1__%2").arg(clean(server), clean(tool));
    if (name.size() > 64) // keep it unique with a short hash of the full name
        name = name.left(55) + QLatin1Char('_')
             + QString::fromLatin1(QCryptographicHash::hash((server + QLatin1Char('/') + tool).toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return name;
}

// The APIs want an object schema; the JSON Schema meta keys are of no use to them.
QJsonObject cleanSchema(QJsonObject schema)
{
    schema.remove(QStringLiteral("$schema"));
    schema.remove(QStringLiteral("$id"));
    if (schema.value(QStringLiteral("type")).toString() != QLatin1String("object"))
        schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    if (!schema.value(QStringLiteral("properties")).isObject())
        schema.insert(QStringLiteral("properties"), QJsonObject());
    return schema;
}

QString firstLine(const QString &text)
{
    const QString line = text.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
    return line.size() > 100 ? line.left(97) + QStringLiteral("…") : line;
}

// A result that is JSON (possibly followed by prose, as Blender's status is) as one readable line: the top-level
// values that are plain ("up_to_date: true · addon_version: 1.8"), or the item count of a list. Empty if not JSON.
QString jsonLine(const QString &text)
{
    const QByteArray raw = text.trimmed().toUtf8();
    if (!raw.startsWith('{') && !raw.startsWith('['))
        return QString();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error == QJsonParseError::GarbageAtEnd)
        doc = QJsonDocument::fromJson(raw.left(err.offset), &err);
    if (err.error != QJsonParseError::NoError)
        return QString();
    if (doc.isArray())
        return Manager::tr("%n item(s)", nullptr, int(doc.array().size()));
    auto plain = [](const QJsonValue &v) -> QString {
        if (v.isString())
            return v.toString().simplified();
        if (v.isBool())
            return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        if (v.isDouble())
            return QString::number(v.toDouble());
        if (v.isArray()) { // short lists of plain values: a version such as [1, 8]
            QStringList items;
            for (const QJsonValue &x : v.toArray()) {
                if (x.isObject() || x.isArray() || items.size() == 4)
                    return QString();
                items << (x.isString() ? x.toString() : QString::number(x.toDouble()));
            }
            return items.join(QLatin1Char('.'));
        }
        return QString();
    };
    QStringList parts;
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end() && parts.size() < 4; ++it) {
        const QString v = plain(it.value());
        if (!v.isEmpty())
            parts << QStringLiteral("%1: %2").arg(it.key(), v.size() > 30 ? v.left(29) + QStringLiteral("…") : v);
    }
    return parts.isEmpty() ? Manager::tr("%n field(s)", nullptr, int(o.size())) : parts.join(QStringLiteral(" · "));
}
} // namespace

Manager::Manager(QObject *parent)
    : QObject(parent)
{
}

Manager::~Manager()
{
    qDeleteAll(m_clients); // each ends its process group
    m_clients.clear();
}

void Manager::setProject(const QString &root, const QString &dataDir)
{
    if (root == m_root && dataDir == m_dataDir)
        return;
    qDeleteAll(m_clients);
    m_clients.clear();
    m_root = root;
    m_dataDir = dataDir;
    m_configs = root.isEmpty() ? QVector<ServerConfig>() : load(dataDir);
    sync();
    emit changed();
}

bool Manager::apply(const QVector<ServerConfig> &configs, QString *error)
{
    if (!save(configs, m_dataDir, error))
        return false;
    m_configs = configs;
    sync();
    emit changed();
    return true;
}

void Manager::setEnabled(const QString &name, bool enabled)
{
    QVector<ServerConfig> list = m_configs;
    for (ServerConfig &s : list)
        if (s.name == name)
            s.enabled = enabled;
    QString error;
    apply(list, &error);
}

void Manager::reconnect(const QString &name)
{
    if (Client *c = m_clients.value(name))
        c->start();
}

void Manager::sync()
{
    QSet<QString> wanted;
    for (const ServerConfig &s : std::as_const(m_configs)) {
        if (!s.enabled || m_root.isEmpty())
            continue;
        wanted.insert(s.name);
        Client *c = m_clients.value(s.name);
        if (c && c->config().sameLaunch(s)) {
            c->setConfig(s);
            continue;
        }
        delete c;
        c = new Client(s, m_root, this);
        connect(c, &Client::changed, this, &Manager::changed);
        m_clients.insert(s.name, c);
        c->start();
    }
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        delete it.value();
        it = m_clients.erase(it);
    }
}

QJsonArray Manager::status() const
{
    QJsonArray out;
    for (const ServerConfig &s : m_configs) {
        QJsonObject o{{"name", s.name}, {"transport", "stdio"}, {"scope", s.global ? tr("all projects") : tr("this project")}};
        const Client *c = m_clients.value(s.name);
        QString status = QStringLiteral("disabled");
        if (c) {
            switch (c->state()) {
            case Client::State::Connected: status = QStringLiteral("connected"); break;
            case Client::State::Starting: status = QStringLiteral("pending"); break;
            case Client::State::Failed: status = QStringLiteral("failed"); break;
            case Client::State::Stopped: status = QStringLiteral("disabled"); break;
            }
            if (!c->error().isEmpty() && c->state() == Client::State::Failed)
                o.insert("error", c->error());
            QJsonArray tools;
            for (const QJsonValue &t : c->tools())
                tools.append(QJsonObject{{"name", t.toObject().value("name")}});
            o.insert("tools", tools);
        }
        o.insert("status", status);
        out.append(o);
    }
    return out;
}

int Manager::connectedCount() const
{
    int n = 0;
    for (const Client *c : m_clients)
        n += c->state() == Client::State::Connected;
    return n;
}

QVector<Tool> Manager::tools() const
{
    QVector<Tool> out;
    QSet<QString> names;
    for (const ServerConfig &s : m_configs) {
        const Client *c = m_clients.value(s.name);
        if (!c || c->state() != Client::State::Connected)
            continue;
        for (const QJsonValue &v : c->tools()) {
            const QJsonObject t = v.toObject();
            Tool tool;
            tool.server = s.name;
            tool.name = t.value("name").toString();
            tool.qualified = qualifiedName(s.name, tool.name);
            if (tool.name.isEmpty() || names.contains(tool.qualified))
                continue;
            names.insert(tool.qualified);
            const QString title = t.value("title").toString(t.value("annotations").toObject().value("title").toString());
            tool.description = t.value("description").toString().trimmed();
            if (tool.description.isEmpty())
                tool.description = title.isEmpty() ? tool.name : title;
            tool.description = QStringLiteral("[MCP server %1] %2").arg(s.name, tool.description.left(4000));
            tool.schema = cleanSchema(t.value("inputSchema").toObject());
            tool.readOnly = t.value("annotations").toObject().value("readOnlyHint").toBool();
            out << tool;
        }
    }
    return out;
}

bool Manager::trusted(const QString &server) const
{
    for (const ServerConfig &s : m_configs)
        if (s.name == server)
            return s.trusted;
    return false;
}

QString Manager::promptSection() const
{
    QStringList connected, missing;
    for (const ServerConfig &s : m_configs) {
        const Client *c = m_clients.value(s.name);
        if (!s.enabled)
            continue;
        if (c && c->state() == Client::State::Connected) {
            QString line = QStringLiteral("- %1").arg(s.name);
            if (!c->serverTitle().isEmpty())
                line += QStringLiteral(" (%1)").arg(c->serverTitle());
            line += QStringLiteral(": %1 tools").arg(c->tools().size());
            const QString instructions = c->instructions().trimmed();
            if (!instructions.isEmpty())
                line += QStringLiteral(". Its instructions:\n%1").arg(instructions.left(kMaxInstructionChars));
            connected << line;
        } else {
            const QString why = !c ? QStringLiteral("not running")
                              : c->state() == Client::State::Starting ? QStringLiteral("still starting")
                                                                      : QStringLiteral("failed: %1").arg(c->error().left(200));
            missing << QStringLiteral("- %1 (%2)").arg(s.name, why);
        }
    }
    if (connected.isEmpty() && missing.isEmpty())
        return {};
    QString text = QStringLiteral("\n\nMCP servers\n");
    if (!connected.isEmpty())
        text += QStringLiteral("The user connected these MCP servers. Their tools are named mcp__<server>__<tool>. They work outside the "
                               "project (in other programs or services), so use them when the request is about what they serve; the "
                               "project tools stay the way to read and edit the project's files.\n")
              + connected.join(QLatin1Char('\n')) + QLatin1Char('\n');
    if (!missing.isEmpty())
        text += QStringLiteral("Configured but not available right now (their tools are missing; if the request needs one, tell the user to "
                               "check it with /mcp):\n")
              + missing.join(QLatin1Char('\n')) + QLatin1Char('\n');
    return text;
}

CallResult Manager::call(const Tool &tool, const QJsonObject &args, const std::function<bool()> &cancelled)
{
    CallResult r;
    Client *c = m_clients.value(tool.server);
    QString error;
    const QJsonObject result = c ? c->callTool(tool.name, args, kCallTimeoutMs, cancelled, &error)
                                 : (error = tr("the MCP server %1 is not running").arg(tool.server), QJsonObject());
    if (!error.isEmpty()) {
        r.response = QJsonObject{{"error", error}};
        r.summary = firstLine(error);
        r.output = error;
        return r;
    }
    QStringList parts;
    int images = 0;
    for (const QJsonValue &v : result.value("content").toArray()) {
        const QJsonObject item = v.toObject();
        const QString type = item.value("type").toString();
        if (type == QLatin1String("text")) {
            parts << item.value("text").toString();
        } else if (type == QLatin1String("image")) {
            const QString mime = item.value("mimeType").toString();
            const QString data = item.value("data").toString();
            static const QStringList kinds{QStringLiteral("image/png"), QStringLiteral("image/jpeg"), QStringLiteral("image/gif"),
                                           QStringLiteral("image/webp")};
            if (kinds.contains(mime) && data.size() <= kMaxImageBase64 && images < 4) {
                r.images.append(QJsonObject{{"mimeType", mime}, {"data", data}});
                parts << QStringLiteral("[image %1 attached]").arg(++images);
            } else {
                parts << QStringLiteral("[an image (%1, %2 KB) was left out]").arg(mime).arg(data.size() * 3 / 4 / 1024);
            }
        } else if (type == QLatin1String("resource")) {
            const QJsonObject res = item.value("resource").toObject();
            if (res.contains("text"))
                parts << QStringLiteral("[%1]\n%2").arg(res.value("uri").toString(), res.value("text").toString());
            else
                parts << QStringLiteral("[binary resource %1 (%2) left out]").arg(res.value("uri").toString(), res.value("mimeType").toString());
        } else if (type == QLatin1String("resource_link")) {
            parts << QStringLiteral("[link] %1 %2").arg(item.value("name").toString(), item.value("uri").toString());
        } else if (!type.isEmpty()) {
            parts << QStringLiteral("[%1 content left out]").arg(type);
        }
    }
    QString text = parts.join(QStringLiteral("\n\n"));
    if (text.trimmed().isEmpty() && result.value("structuredContent").isObject())
        text = QString::fromUtf8(QJsonDocument(result.value("structuredContent").toObject()).toJson(QJsonDocument::Compact));
    r.output = text;
    if (text.size() > kMaxTextChars)
        text = text.left(kMaxTextChars) + QStringLiteral("\n… (cut: %1 more characters)").arg(text.size() - kMaxTextChars);
    r.ok = !result.value("isError").toBool();
    r.response = QJsonObject{{r.ok ? "output" : "error", text.isEmpty() ? QStringLiteral("(no output)") : text}};
    r.summary = firstLine(jsonLine(text));
    if (r.summary.isEmpty())
        r.summary = firstLine(text);
    if (r.summary.isEmpty())
        r.summary = r.ok ? tr("done") : tr("failed");
    return r;
}

} // namespace mcp
