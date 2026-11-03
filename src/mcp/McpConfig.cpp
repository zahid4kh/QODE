#include "McpConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace mcp {

namespace {
QString globalFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/QODE-AGENTIC/mcp.json");
}

QString projectFile(const QString &dataDir)
{
    return dataDir.isEmpty() ? QString() : dataDir + QStringLiteral("/mcp.json");
}

QJsonObject readObject(const QString &path)
{
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QJsonObject serversOf(const QJsonObject &doc)
{
    return doc.value(QStringLiteral("mcpServers")).toObject();
}

bool writeServers(const QString &path, const QVector<ServerConfig> &servers, QString *error, const QJsonObject &keep = {})
{
    QJsonObject map = keep;
    for (const ServerConfig &s : servers)
        map.insert(s.name, s.toJson());
    if (map.isEmpty() && !QFileInfo::exists(path))
        return true;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = QObject::tr("Cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("mcpServers"), map}}).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        *error = QObject::tr("Cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString home(const QString &path)
{
    const QString h = QDir::homePath();
    return path.startsWith(h + QLatin1Char('/')) ? QStringLiteral("~") + path.mid(h.size()) : path;
}

QString typeOf(const QJsonObject &o)
{
    const QString t = o.value(QStringLiteral("type")).toString();
    if (!t.isEmpty())
        return t;
    return o.contains(QStringLiteral("url")) ? QStringLiteral("http") : QStringLiteral("stdio");
}
} // namespace

QJsonObject ServerConfig::toJson() const
{
    QJsonObject o{{QStringLiteral("command"), command}};
    if (!args.isEmpty())
        o.insert(QStringLiteral("args"), QJsonArray::fromStringList(args));
    if (!env.isEmpty()) {
        QJsonObject e;
        for (auto it = env.cbegin(); it != env.cend(); ++it)
            e.insert(it.key(), it.value());
        o.insert(QStringLiteral("env"), e);
    }
    if (!cwd.isEmpty())
        o.insert(QStringLiteral("cwd"), cwd);
    if (!enabled)
        o.insert(QStringLiteral("disabled"), true);
    if (trusted)
        o.insert(QStringLiteral("trusted"), true);
    return o;
}

ServerConfig ServerConfig::fromJson(const QString &name, const QJsonObject &o)
{
    ServerConfig s;
    s.name = name;
    s.command = o.value(QStringLiteral("command")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("args")).toArray())
        s.args << v.toString();
    const QJsonObject env = o.value(QStringLiteral("env")).toObject();
    for (auto it = env.begin(); it != env.end(); ++it)
        s.env.insert(it.key(), it.value().toString());
    s.cwd = o.value(QStringLiteral("cwd")).toString();
    s.enabled = !o.value(QStringLiteral("disabled")).toBool();
    s.trusted = o.value(QStringLiteral("trusted")).toBool();
    return s;
}

bool ServerConfig::sameLaunch(const ServerConfig &o) const
{
    return command == o.command && args == o.args && env == o.env && cwd == o.cwd;
}

QString ServerConfig::commandLine() const
{
    return joinArgs(QStringList{command} + args);
}

QString joinArgs(const QStringList &args)
{
    QStringList parts;
    for (const QString &a : args) {
        if (a.isEmpty() || a.contains(QRegularExpression(QStringLiteral("[\\s\"']")))) {
            QString q = a;
            q.replace(QLatin1Char('"'), QStringLiteral("\"\"\""));
            parts << QLatin1Char('"') + q + QLatin1Char('"');
        } else {
            parts << a;
        }
    }
    return parts.join(QLatin1Char(' '));
}

QVector<ServerConfig> load(const QString &projectDataDir)
{
    QVector<ServerConfig> out;
    QStringList projectNames;
    const QJsonObject project = serversOf(readObject(projectFile(projectDataDir)));
    for (auto it = project.begin(); it != project.end(); ++it)
        projectNames << it.key();
    const QJsonObject global = serversOf(readObject(globalFile()));
    for (auto it = global.begin(); it != global.end(); ++it) {
        if (projectNames.contains(it.key()))
            continue;
        ServerConfig s = ServerConfig::fromJson(it.key(), it.value().toObject());
        s.global = true;
        out << s;
    }
    for (auto it = project.begin(); it != project.end(); ++it)
        out << ServerConfig::fromJson(it.key(), it.value().toObject());
    return out;
}

bool save(const QVector<ServerConfig> &servers, const QString &projectDataDir, QString *error)
{
    QVector<ServerConfig> global, project;
    for (const ServerConfig &s : servers)
        (s.global || projectDataDir.isEmpty() ? global : project) << s;
    // A global server hidden by a project server of the same name was not in the list: it stays.
    QJsonObject hidden;
    const QJsonObject old = serversOf(readObject(globalFile()));
    for (const ServerConfig &s : std::as_const(project))
        if (old.contains(s.name) && std::none_of(global.cbegin(), global.cend(), [&s](const ServerConfig &g) { return g.name == s.name; }))
            hidden.insert(s.name, old.value(s.name));
    if (!writeServers(globalFile(), global, error, hidden))
        return false;
    return projectDataDir.isEmpty() || writeServers(projectFile(projectDataDir), project, error);
}

QVector<Found> claudeCodeServers(const QString &projectRoot)
{
    QVector<Found> out;
    auto add = [&out](const QJsonObject &map, const QString &where) {
        for (auto it = map.begin(); it != map.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            Found f;
            f.config = ServerConfig::fromJson(it.key(), o);
            f.config.enabled = true;
            f.where = where;
            f.type = typeOf(o);
            out << f;
        }
    };
    const QJsonObject claude = readObject(QDir::homePath() + QStringLiteral("/.claude.json"));
    add(serversOf(claude), QObject::tr("user settings"));
    if (!projectRoot.isEmpty())
        add(serversOf(readObject(projectRoot + QStringLiteral("/.mcp.json"))), QObject::tr("this project's .mcp.json"));
    const QJsonObject projects = claude.value(QStringLiteral("projects")).toObject();
    // The open project first, then the others (a server set up for another folder works here too).
    QStringList roots = projects.keys();
    if (roots.removeOne(projectRoot))
        roots.prepend(projectRoot);
    for (const QString &root : std::as_const(roots))
        add(serversOf(projects.value(root).toObject()),
            root == projectRoot ? QObject::tr("this project") : QObject::tr("project %1").arg(home(root)));
    return out;
}

QVector<ServerConfig> parseSnippet(const QString &text, QString *error)
{
    // READMEs often show the snippet with a trailing comma or wrapped in a bare "mcpServers": {...} member.
    QString t = text.trimmed();
    while (t.endsWith(QLatin1Char(',')))
        t.chop(1);
    if (t.startsWith(QLatin1Char('"')))
        t = QLatin1Char('{') + t + QLatin1Char('}');
    t.remove(QRegularExpression(QStringLiteral(",\\s*(?=[}\\]])")));
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(t.toUtf8(), &pe);
    if (!doc.isObject()) {
        *error = QObject::tr("This is not valid JSON: %1.").arg(pe.errorString());
        return {};
    }
    QJsonObject root = doc.object();
    for (const char *key : {"mcpServers", "servers", "mcp"}) // Claude / Cursor, VS Code, and "mcp": {"servers": ...}
        if (root.value(QLatin1String(key)).isObject()) {
            root = root.value(QLatin1String(key)).toObject();
            if (root.value(QStringLiteral("servers")).isObject())
                root = root.value(QStringLiteral("servers")).toObject();
            break;
        }
    QVector<ServerConfig> out;
    if (root.contains(QStringLiteral("command"))) {
        out << ServerConfig::fromJson(QString(), root);
    } else {
        for (auto it = root.begin(); it != root.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            if (o.contains(QStringLiteral("command")))
                out << ServerConfig::fromJson(it.key(), o);
            else if (o.contains(QStringLiteral("url")))
                *error = QObject::tr("“%1” is an HTTP server; QODE runs local (stdio) servers only.").arg(it.key());
        }
    }
    if (out.isEmpty() && error->isEmpty())
        *error = QObject::tr("No server with a \"command\" was found in the text.");
    return out;
}

bool validName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,40}$"));
    return re.match(name).hasMatch();
}

QString expand(const QString &text, const QProcessEnvironment &env)
{
    static const QRegularExpression re(QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\\}"));
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QString value = env.value(m.captured(1));
        out += value.isEmpty() ? m.captured(2) : value;
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

QProcessEnvironment environment(const ServerConfig &config)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList path = env.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    const QString h = QDir::homePath();
    const QStringList extra{h + QStringLiteral("/.local/bin"), h + QStringLiteral("/.cargo/bin"), h + QStringLiteral("/.bun/bin"),
                            h + QStringLiteral("/.deno/bin"), h + QStringLiteral("/.npm-global/bin"), QStringLiteral("/usr/local/bin")};
    for (const QString &dir : extra)
        if (!path.contains(dir) && QFileInfo(dir).isDir())
            path << dir;
    env.insert(QStringLiteral("PATH"), path.join(QLatin1Char(':')));
    const QProcessEnvironment base = env;
    for (auto it = config.env.cbegin(); it != config.env.cend(); ++it)
        env.insert(it.key(), expand(it.value(), base));
    return env;
}

} // namespace mcp
