#pragma once

// Helpers shared by the files that define AgentTools' tools (AgentTools.cpp, AgentToolsServers.cpp, ...).
#include "AgentTools.h"
#include "platform/Platform.h"

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

// The last `limit` bytes of a command's output as text (decoded as Platform::decodeOutput does).
inline QString clipTail(const QByteArray &data, int limit = 8000)
{
    if (data.size() <= limit)
        return Platform::decodeOutput(data);
    // Start at a whole UTF-8 character: half of one would make the tail invalid UTF-8 (and on Windows read as ANSI).
    int from = int(data.size()) - limit;
    for (int i = 0; i < 3 && from < data.size() && (uchar(data.at(from)) & 0xC0) == 0x80; ++i)
        ++from;
    return QStringLiteral("...(earlier output cut)...\n") + Platform::decodeOutput(data.mid(from));
}

// Windows' destructive commands (PowerShell, cmd), checked in each part of the line between | ; & and line breaks: a
// recursive delete of a drive root, the home folder, the Windows folder or the current folder or its parent.
inline QString windowsDeleteDenied(const QString &command)
{
    static const QRegularExpression parts(QStringLiteral("[|;&\\n]"));
    static const QRegularExpression verb(QStringLiteral(R"(^\s*(?:Remove-Item|rm|ri|del|erase|rd|rmdir)\b)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression recursive(QStringLiteral(R"((?:^|\s)(?:-r\w*|/s)(?=\s|$))"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression target(
        QStringLiteral(R"((?:^|\s)['"]?(?:[a-z]:[\\/]?\*?|[\\/]\*?|\*|~[\\/]?\*?|\$HOME[\\/]?\*?|\$env:(?:USERPROFILE|HOMEDRIVE|SystemRoot|SystemDrive|windir|APPDATA|LOCALAPPDATA)[\\/]?\*?|%(?:USERPROFILE|SystemRoot|SystemDrive|windir|APPDATA|LOCALAPPDATA)%[\\/]?\*?|\.\.?[\\/]?\*?)['"]?(?=\s|$))"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &part : command.split(parts))
        if (verb.match(part).hasMatch() && recursive.match(part).hasMatch() && target.match(part).hasMatch())
            return QStringLiteral("recursive delete of a drive, the home folder or the project root is not allowed");
    return {};
}

// Rejects the commands no coding task needs: privilege escalation, disk tools, power commands, piping downloads into a shell.
inline QString commandDenied(const QString &command)
{
    struct Rule {
        const char *pattern, *reason;
    };
    static const Rule rules[] = {
        {R"(\bsudo\b|\bsu\s|\bpkexec\b|\bdoas\b)", "privileged commands (sudo/su/pkexec) are not allowed"},
        {R"(\brm\b[^|;&]*\s-[a-zA-Z]*[rR][a-zA-Z]*[^|;&]*\s(/|~|\$HOME|/\*|~/\*|\.\.?)(\s|$))", "recursive delete of /, home or the project root is not allowed"},
        {R"(\bmkfs(\.\w+)?\b|\bwipefs\b|\bfdisk\b|\bparted\b)", "disk formatting is not allowed"},
        {R"(\bdd\b[^|;&]*\bof=/dev/)", "writing to block devices is not allowed"},
        {R"(\b(shutdown|reboot|poweroff|halt)\b|systemctl\s+(poweroff|reboot|halt))", "power commands are not allowed"},
        {R"(:\(\)\s*\{)", "fork bombs are not allowed"},
        {R"((curl|wget)\b[^|;&]*\|\s*(sudo\s+)?(ba|z)?sh\b)", "piping downloads into a shell is not allowed"},
        {R"(\.ssh\b|\.gnupg\b|\.aws\b)", "commands touching credential locations are not allowed"},
        {R"(\bgit\s+push\b)", "pushing is left to the user"},
        // QODE's own API keys (claude.key / gemini.key in its config folder).
        {R"((?i)\b(claude|gemini)\.key\b|QODE-AGENTIC(?=[\\/'"\s]|$))", "QODE's own settings and API keys are off limits"},
        // Windows (PowerShell and cmd ignore case).
        {R"((?i)\brunas\b|-Verb\s+['"]?RunAs\b|\bgsudo\b)", "privileged commands (runas, sudo) are not allowed"},
        {R"((?i)\bformat\s+[a-z]:|\bdiskpart\b|\b(Format-Volume|Clear-Disk|Initialize-Disk|Remove-Partition)\b|\bcipher\s+/w\b|\bbcdedit\b)",
         "disk formatting is not allowed"},
        {R"((?i)\b(Stop-Computer|Restart-Computer)\b)", "power commands are not allowed"},
        {R"((?i)\b(iwr|irm|curl|wget|Invoke-WebRequest|Invoke-RestMethod)\b[^|;&]*\|\s*(iex|Invoke-Expression)\b|\b(iex|Invoke-Expression)\b[^|;&]*\b(iwr|irm|Invoke-WebRequest|Invoke-RestMethod|DownloadString)\b)",
         "running downloaded scripts is not allowed"},
        {R"((?i)Microsoft[\\/](Credentials|Protect|Vault)\b|\bcmdkey\b|\bvaultcmd\b|\.azure\b)", "commands touching credential locations are not allowed"},
        {R"((?i)\breg(\.exe)?\s+(delete|add|import)\s+['"]?HK(LM|EY_LOCAL_MACHINE)|\b(Remove-Item|Set-ItemProperty|New-ItemProperty|Remove-ItemProperty)\b[^|;&]*\bHKLM:)",
         "changing the system registry is not allowed"},
        {R"((?i)\b(Set-MpPreference|Add-MpPreference)\b|\bnetsh\s+advfirewall\b)", "changing Windows security settings is not allowed"},
    };
    for (const Rule &r : rules)
        if (QRegularExpression(QString::fromLatin1(r.pattern)).match(command).hasMatch())
            return QString::fromLatin1(r.reason);
    return windowsDeleteDenied(command);
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
