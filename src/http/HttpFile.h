#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

// .http / .rest files (the format of VS Code's REST Client and IntelliJ's HTTP client): requests separated by ###, each a
// request line, headers, a blank line and a body. This part only reads the text; HttpVars fills in {{variables}}.

using HttpHeaders = QList<QPair<QString, QString>>;

// One request of a file, as written (variables not filled in yet).
struct HttpRequestSpec
{
    int firstLine = 0, lastLine = 0; // 0-based block range: the ### line through the line before the next ###
    int line = -1;                   // the request line (METHOD URL); -1 when the block has none (comments only)
    QString title;                   // text after ###
    QString name;                    // # @name, the handle other requests use: {{name.response.body.$.id}}
    QString method = QStringLiteral("GET");
    QString url;
    HttpHeaders headers;
    QStringList bodyLines;           // raw; "< ./file" lines are read when the request is built
    QString saveTo;                  // ">> file" / ">>! file" after the body
    bool overwrite = false;          // ">>!"
    bool noRedirect = false, noCookieJar = false;
    bool handlerScript = false;      // IntelliJ "> {% … %}" or "> file.js": not run, only reported
    int timeoutMs = 0;               // # @timeout; 0 = the default
    QList<QPair<QString, QString>> prompts; // # @prompt variable description

    QString label() const;           // name, ### title, or METHOD url
};

struct HttpFileVar
{
    QString name, value;
    int line = 0;
};

struct HttpFileParse
{
    QVector<HttpRequestSpec> requests; // only blocks with a request line
    QVector<HttpFileVar> variables;    // @name = value, in file order

    int requestAt(int line) const;     // the request whose block holds `line`, -1 for none
    int indexOfName(const QString &name) const;
    QList<int> requestLines() const;
};

// A request ready to go out: variables filled in, files read.
struct HttpRequestData
{
    QString method = QStringLiteral("GET");
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    bool followRedirects = true, useCookies = true;
    int timeoutMs = 60000;
};

// What came back (or why nothing did).
struct HttpResult
{
    QString name, label;             // of the request in its file
    QString file;                    // the .http file ("" when sent from elsewhere)
    int requestLine = -1;
    HttpRequestData request;
    int status = 0;                  // 0: no response, see error
    QString reason;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    qint64 ms = 0;
    int error = 0;                   // QNetworkReply::NetworkError
    QString errorText;
    QDateTime time;
    QString note;                    // a remark shown with the status (handler scripts skipped, saved to file …)

    bool ok() const { return status > 0; }
    QByteArray header(const QByteArray &name) const; // case-insensitive, "" when missing
    QString contentType() const;     // lower case, without parameters
};

namespace HttpFile {

bool isHttpFile(const QString &path); // .http / .rest
HttpFileParse parse(const QString &text);
// A request as .http text, for copying out of the Spring client. `headers` is "Name: value" lines.
QString format(const QString &name, const QString &method, const QString &url, const QString &headers, const QString &body);
const QStringList &methods();

} // namespace HttpFile
