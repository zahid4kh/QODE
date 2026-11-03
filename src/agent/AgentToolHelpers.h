#pragma once

// Helpers shared by the files that define AgentTools' tools (AgentTools.cpp, AgentToolsServers.cpp, ...).
#include "AgentTools.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <functional>
#include <initializer_list>

namespace agent {

struct Param {
    const char *name;
    const char *type; // string | integer | boolean | array (of strings)
    const char *description;
    bool required = true;
};

inline QJsonObject makeSchema(std::initializer_list<Param> params)
{
    QJsonObject props;
    QJsonArray required;
    for (const Param &p : params) {
        QJsonObject prop{{"type", QString::fromLatin1(p.type)}, {"description", QString::fromLatin1(p.description)}};
        if (QLatin1String(p.type) == QLatin1String("array"))
            prop.insert("items", QJsonObject{{"type", "string"}});
        props.insert(QString::fromLatin1(p.name), prop);
        if (p.required)
            required.append(QString::fromLatin1(p.name));
    }
    QJsonObject schema{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty())
        schema.insert("required", required);
    return schema;
}

inline ToolResult fail(const QString &message)
{
    ToolResult r;
    r.ok = false;
    r.response = QJsonObject{{"error", message}};
    r.summary = message;
    return r;
}

inline ToolResult success(const QJsonObject &response, const QString &summary)
{
    ToolResult r;
    r.response = response;
    r.summary = summary;
    return r;
}

inline QString str(const QJsonObject &a, const char *key)
{
    return a.value(QLatin1String(key)).toString();
}

inline QString plural(int n, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
}

inline QString shorten(const QString &s, int n)
{
    const QString one = s.simplified();
    return one.size() > n ? one.left(n - 1) + QChar(0x2026) : one;
}

// Adds an enum to one string property of a schema made by makeSchema.
inline QJsonObject withEnum(QJsonObject schema, const char *property, const QStringList &values)
{
    QJsonObject props = schema.value("properties").toObject();
    QJsonObject prop = props.value(QLatin1String(property)).toObject();
    prop.insert("enum", QJsonArray::fromStringList(values));
    props.insert(QLatin1String(property), prop);
    schema.insert("properties", props);
    return schema;
}

// Masks what looks like a secret in text the model gets (terminal output, HTTP responses): KEY=value / token: value pairs and well-known key formats.
inline QString maskSecrets(QString text)
{
    // key = value / "key": "value" pairs; the value may start with an auth scheme ("Bearer abc").
    static const QRegularExpression pairs(
        QStringLiteral(R"(((?:api[_-]?key|access[_-]?key|secret|token|passw(?:or)?d|authori[sz]ation|auth(?!or)|credential|private[_-]?key|cookie|session)\w*["']?\s*[=:]\s*["']?)(?:(?:Bearer|Basic|Token|Digest)\s+)?[^\s"',;\\]{3,})"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression schemes(QStringLiteral(R"(\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{6,})"));
    static const QRegularExpression keys(QStringLiteral(
        R"(\b(?:sk-[A-Za-z0-9_-]{16,}|sk-ant-[A-Za-z0-9_-]{16,}|AIza[0-9A-Za-z_-]{30,}|gh[pousr]_[A-Za-z0-9]{30,}|xox[abpr]-[A-Za-z0-9-]{10,}|AKIA[0-9A-Z]{16})\b)"));
    text.replace(pairs, QStringLiteral("\\1[hidden]"));
    text.replace(schemes, QStringLiteral("\\1 [hidden]"));
    text.replace(keys, QStringLiteral("[hidden]"));
    return text;
}

// Runs the event loop until `done` returns true (checked every `pollMs`), the time is up or `cancelled` is set (Stop).
// Returns whether `done` became true.
inline bool waitUntil(const std::function<bool()> &done, int timeoutMs, const bool &cancelled, int pollMs = 250)
{
    if (done())
        return true;
    bool ok = false;
    QEventLoop loop;
    QTimer poll;
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done()) {
            ok = true;
            loop.quit();
        } else if (cancelled || clock.elapsed() >= timeoutMs) {
            loop.quit();
        }
    });
    poll.start(pollMs);
    loop.exec();
    return ok;
}

} // namespace agent
