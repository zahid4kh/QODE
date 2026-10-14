#include "AgentSession.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>

namespace agent {

namespace {
QString kindName(Block::Kind k)
{
    return k == Block::Thought ? QStringLiteral("thought") : k == Block::Tool ? QStringLiteral("tool") : QStringLiteral("text");
}
}

QJsonObject Block::toJson() const
{
    QJsonObject o{{QStringLiteral("kind"), kindName(kind)}, {QStringLiteral("text"), text}};
    if (kind == Tool) {
        o.insert(QStringLiteral("name"), name);
        o.insert(QStringLiteral("ok"), ok);
        o.insert(QStringLiteral("summary"), summary);
        if (hasChange)
            o.insert(QStringLiteral("change"), change.toJson());
    }
    return o;
}

Block Block::fromJson(const QJsonObject &o)
{
    Block b;
    const QString k = o.value(QStringLiteral("kind")).toString();
    b.kind = k == QLatin1String("thought") ? Thought : k == QLatin1String("tool") ? Tool : Text;
    b.text = o.value(QStringLiteral("text")).toString();
    if (b.kind == Tool) {
        b.name = o.value(QStringLiteral("name")).toString();
        b.done = true; // a saved tool call is over, even if the run was cut short
        b.ok = o.value(QStringLiteral("ok")).toBool();
        b.summary = o.value(QStringLiteral("summary")).toString();
        if (o.contains(QStringLiteral("change"))) {
            b.hasChange = true;
            b.change = AiChange::fromJson(o.value(QStringLiteral("change")).toObject());
        }
    }
    return b;
}

QString Message::answer() const
{
    QStringList parts;
    for (const Block &b : blocks)
        if (b.kind == Block::Text && !b.text.trimmed().isEmpty())
            parts << b.text.trimmed();
    return parts.join(QStringLiteral("\n\n"));
}

QString Message::actions() const
{
    QStringList edited, other;
    for (const Block &b : blocks) {
        if (b.kind != Block::Tool || !b.ok)
            continue;
        if (b.hasChange) {
            if (!edited.contains(b.change.path))
                edited << b.change.path;
        } else if (b.name == QLatin1String("run_command") || b.name == QLatin1String("delete_path") || b.name == QLatin1String("move_path")
                   || b.name == QLatin1String("copy_path") || b.name == QLatin1String("create_directory")) {
            other << b.text;
        }
    }
    QStringList out;
    if (!edited.isEmpty())
        out << QStringLiteral("changed files: %1").arg(edited.join(QStringLiteral(", ")));
    if (!other.isEmpty())
        out << QStringLiteral("also: %1").arg(other.mid(0, 12).join(QStringLiteral("; ")));
    return out.join(QStringLiteral("; "));
}

QJsonObject Message::toJson() const
{
    QJsonObject o{{QStringLiteral("role"), role}, {QStringLiteral("text"), text}, {QStringLiteral("time"), time.toString(Qt::ISODate)}};
    if (!files.isEmpty())
        o.insert(QStringLiteral("files"), QJsonArray::fromStringList(files));
    if (!blocks.isEmpty()) {
        QJsonArray a;
        for (const Block &b : blocks)
            a.append(b.toJson());
        o.insert(QStringLiteral("blocks"), a);
    }
    if (!model.isEmpty())
        o.insert(QStringLiteral("model"), model);
    return o;
}

Message Message::fromJson(const QJsonObject &o)
{
    Message m;
    m.role = o.value(QStringLiteral("role")).toString();
    m.text = o.value(QStringLiteral("text")).toString();
    m.time = QDateTime::fromString(o.value(QStringLiteral("time")).toString(), Qt::ISODate);
    m.model = o.value(QStringLiteral("model")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray())
        m.files << v.toString();
    for (const QJsonValue &v : o.value(QStringLiteral("blocks")).toArray())
        m.blocks.append(Block::fromJson(v.toObject()));
    return m;
}

Session Session::create(const QString &model)
{
    Session s;
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + QLatin1Char('-') + QString::number(QDateTime::currentSecsSinceEpoch(), 36);
    s.model = model;
    s.created = s.updated = QDateTime::currentDateTime();
    return s;
}

QJsonArray Session::history() const
{
    struct Turn {
        QString role, text;
    };
    QVector<Turn> turns;
    auto add = [&turns](const QString &role, const QString &text) {
        if (text.trimmed().isEmpty())
            return;
        if (!turns.isEmpty() && turns.last().role == role)
            turns.last().text += QStringLiteral("\n\n") + text; // the API wants the roles to alternate
        else
            turns.append({role, text});
    };
    for (const Message &m : messages) {
        if (m.role == QLatin1String("user")) {
            QString text = m.text;
            if (!m.files.isEmpty())
                text += QStringLiteral("\n\n(pointed at: %1)").arg(m.files.join(QStringLiteral(", ")));
            add(QStringLiteral("user"), text);
        } else if (m.role == QLatin1String("assistant")) {
            QString text = m.answer();
            const QString acts = m.actions();
            if (!acts.isEmpty())
                text += QStringLiteral("\n\n(%1)").arg(acts);
            add(QStringLiteral("model"), text);
        }
    }
    while (!turns.isEmpty() && turns.first().role != QLatin1String("user"))
        turns.removeFirst();
    QJsonArray out;
    for (const Turn &t : turns)
        out.append(QJsonObject{{QStringLiteral("role"), t.role},
                               {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), t.text}}}}});
    return out;
}

QJsonObject Session::toJson() const
{
    QJsonArray msgs;
    for (const Message &m : messages)
        msgs.append(m.toJson());
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("kind"), QStringLiteral("agent")},
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), title},
            {QStringLiteral("model"), model},
            {QStringLiteral("created"), created.toString(Qt::ISODate)},
            {QStringLiteral("updated"), updated.toString(Qt::ISODate)},
            {QStringLiteral("messages"), msgs}};
}

Session Session::fromJson(const QJsonObject &o)
{
    Session s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.title = o.value(QStringLiteral("title")).toString();
    s.model = o.value(QStringLiteral("model")).toString();
    s.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
    s.updated = QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODate);
    for (const QJsonValue &v : o.value(QStringLiteral("messages")).toArray())
        s.messages.append(Message::fromJson(v.toObject()));
    return s;
}

QString Store::folder() const
{
    return m_dir + QStringLiteral("/agent-chats");
}

QString Store::pathOf(const QString &id) const
{
    QString safe = id;
    safe.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")));
    return folder() + QLatin1Char('/') + safe + QStringLiteral(".json");
}

QVector<Store::Info> Store::list() const
{
    QVector<Info> out;
    if (!valid())
        return out;
    const QDir dir(folder());
    for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.isEmpty())
            continue;
        out.append({o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("title")).toString(),
                    QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODate)});
    }
    std::sort(out.begin(), out.end(), [](const Info &a, const Info &b) { return a.updated > b.updated; });
    return out;
}

bool Store::load(const QString &id, Session *out) const
{
    QFile f(pathOf(id));
    if (!valid() || !f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty())
        return false;
    *out = Session::fromJson(o);
    return !out->id.isEmpty();
}

bool Store::save(const Session &session) const
{
    if (!valid())
        return false;
    QDir().mkpath(folder());
    QSaveFile f(pathOf(session.id));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(session.toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool Store::remove(const QString &id) const
{
    return valid() && QFile::remove(pathOf(id));
}

} // namespace agent
