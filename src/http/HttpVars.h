#pragma once

#include "http/HttpFile.h"

#include <QHash>

// Where the {{variables}} of a request come from when it is sent. Precedence: @prompt answers, then the file's
// @variables, then the selected environment. {{name.response.body.$.id}} reads the last response of the request named
// `name` in the same file; {{$guid}}, {{$timestamp}}, {{$datetime …}}, {{$processEnv X}}, {{$dotenv X}} … are made on
// the spot.
struct HttpContext
{
    QString dir;                              // folder of the .http file: relative paths and .env
    QString envName;                          // shown in messages
    QHash<QString, QString> fileVars, env, prompts, dotenv;
    QHash<QString, HttpResult> responses;     // by request name
    QString springBase;                       // {{$springBase}}: the running Spring Boot application ("" outside one)
};

class HttpResolver
{
public:
    explicit HttpResolver(const HttpContext &ctx) : m_ctx(ctx) {}

    // {{…}} filled in; the ones that cannot be stay as written and are listed in missing().
    QString resolve(const QString &text);
    const QStringList &missing() const { return m_missing; }
    // One expression (the text between {{ and }}); false when unknown. `source` says where the value came from.
    bool lookup(const QString &expr, QString *value, QString *source = nullptr, int depth = 0);

private:
    QString resolve(const QString &text, int depth);
    bool system(const QString &expr, QString *value, QString *source, int depth);
    bool fromResponse(const QString &expr, QString *value, QString *source);

    const HttpContext &m_ctx;
    QStringList m_missing;
};

namespace HttpVars {

// The requests this one reads responses of ({{login.response.body.$.token}} -> "login").
QStringList referencedNames(const HttpRequestSpec &spec);
// The request as it goes out. False with *error when something is missing (an unknown variable, an unreadable file);
// *note gets remarks worth showing with the response (a handler script that is not run).
bool build(const HttpRequestSpec &spec, const HttpContext &ctx, HttpRequestData *out, QString *error, QString *note);
// Tooltip for the {{expression}} under `column` of `lineText`; "" when the column is not inside one.
QString describeAt(const QString &lineText, int column, const HttpContext &ctx);
// $.a.b[0] / a.b / * of a JSON document; objects and arrays come back as compact JSON.
QString jsonPath(const QByteArray &json, const QString &path, bool *ok);

} // namespace HttpVars
