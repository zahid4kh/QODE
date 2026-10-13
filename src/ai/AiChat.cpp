#include "AiChat.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

QJsonObject AiChange::toJson() const
{
    return {{QStringLiteral("path"), path}, {QStringLiteral("op"), op}, {QStringLiteral("startLine"), startLine},
            {QStringLiteral("endLine"), endLine}, {QStringLiteral("ok"), ok}, {QStringLiteral("note"), note}};
}

AiChange AiChange::fromJson(const QJsonObject &o)
{
    AiChange c;
    c.path = o.value(QStringLiteral("path")).toString();
    c.op = o.value(QStringLiteral("op")).toString();
    c.startLine = o.value(QStringLiteral("startLine")).toInt();
    c.endLine = o.value(QStringLiteral("endLine")).toInt();
    c.ok = o.value(QStringLiteral("ok")).toBool();
    c.note = o.value(QStringLiteral("note")).toString();
    return c;
}

QJsonObject AiMessage::toJson() const
{
    QJsonObject o{{QStringLiteral("role"), role}, {QStringLiteral("text"), text}, {QStringLiteral("time"), time.toString(Qt::ISODate)}};
    if (!files.isEmpty())
        o.insert(QStringLiteral("files"), QJsonArray::fromStringList(files));
    if (!plan.isEmpty())
        o.insert(QStringLiteral("plan"), QJsonArray::fromStringList(plan));
    if (!changes.isEmpty()) {
        QJsonArray a;
        for (const AiChange &c : changes)
            a.append(c.toJson());
        o.insert(QStringLiteral("changes"), a);
    }
    if (!model.isEmpty())
        o.insert(QStringLiteral("model"), model);
    return o;
}

AiMessage AiMessage::fromJson(const QJsonObject &o)
{
    AiMessage m;
    m.role = o.value(QStringLiteral("role")).toString();
    m.text = o.value(QStringLiteral("text")).toString();
    m.time = QDateTime::fromString(o.value(QStringLiteral("time")).toString(), Qt::ISODate);
    m.model = o.value(QStringLiteral("model")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray())
        m.files << v.toString();
    for (const QJsonValue &v : o.value(QStringLiteral("plan")).toArray())
        m.plan << v.toString();
    for (const QJsonValue &v : o.value(QStringLiteral("changes")).toArray())
        m.changes.append(AiChange::fromJson(v.toObject()));
    return m;
}

AiSession AiSession::create(const QString &model)
{
    AiSession s;
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + QLatin1Char('-') + QString::number(QDateTime::currentSecsSinceEpoch(), 36);
    s.model = model;
    s.created = s.updated = QDateTime::currentDateTime();
    return s;
}

QJsonArray AiSession::history() const
{
    QJsonArray out;
    for (const AiMessage &m : messages) {
        if (m.role != QLatin1String("user") && m.role != QLatin1String("assistant"))
            continue;
        QString text = m.text;
        if (m.role == QLatin1String("user") && !m.files.isEmpty())
            text += QStringLiteral("\n\n(attached: %1)").arg(m.files.join(QStringLiteral(", ")));
        // A reply that edited files also tells the model what changed.
        if (m.role == QLatin1String("assistant") && !m.plan.isEmpty())
            text = QStringLiteral("Plan:\n- %1\n\n%2").arg(m.plan.join(QStringLiteral("\n- ")), text);
        out.append(QJsonObject{{QStringLiteral("role"), m.role == QLatin1String("user") ? QStringLiteral("user") : QStringLiteral("model")},
                               {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), text}}}}});
    }
    return out;
}

QJsonObject AiSession::toJson() const
{
    QJsonArray msgs;
    for (const AiMessage &m : messages)
        msgs.append(m.toJson());
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), title},
            {QStringLiteral("model"), model},
            {QStringLiteral("created"), created.toString(Qt::ISODate)},
            {QStringLiteral("updated"), updated.toString(Qt::ISODate)},
            {QStringLiteral("messages"), msgs}};
}

AiSession AiSession::fromJson(const QJsonObject &o)
{
    AiSession s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.title = o.value(QStringLiteral("title")).toString();
    s.model = o.value(QStringLiteral("model")).toString();
    s.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
    s.updated = QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODate);
    for (const QJsonValue &v : o.value(QStringLiteral("messages")).toArray())
        s.messages.append(AiMessage::fromJson(v.toObject()));
    return s;
}

QString AiChatStore::pathOf(const QString &id) const
{
    QString safe = id;
    safe.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")));
    return m_dir + QStringLiteral("/chats/") + safe + QStringLiteral(".json");
}

QVector<AiChatStore::Info> AiChatStore::list() const
{
    QVector<Info> out;
    if (!valid())
        return out;
    const QDir dir(m_dir + QStringLiteral("/chats"));
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

bool AiChatStore::load(const QString &id, AiSession *out) const
{
    QFile f(pathOf(id));
    if (!valid() || !f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty())
        return false;
    *out = AiSession::fromJson(o);
    return !out->id.isEmpty();
}

bool AiChatStore::save(const AiSession &session) const
{
    if (!valid())
        return false;
    QDir().mkpath(m_dir + QStringLiteral("/chats"));
    QSaveFile f(pathOf(session.id));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(session.toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool AiChatStore::remove(const QString &id) const
{
    return valid() && QFile::remove(pathOf(id));
}
