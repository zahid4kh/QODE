// The "http" tool group: http_request sends a request of a .http / .rest file through QODE's HTTP panel (its variables,
// environments and request chaining, like the user's Send) and returns the response. The window side
// (MainWindowHttp.cpp) starts it; this side polls, so Stop works.
#include "AgentToolHelpers.h"

#include "http/HttpFile.h"

namespace agent {

namespace {
constexpr int kHttpWaitMs = 120000;

// Every string in the object (nested ones too) with secrets masked; masking the serialized JSON instead could eat an
// escape and break it.
QJsonObject maskedJson(const QJsonObject &o)
{
    QJsonObject out;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QJsonValue v = it.value();
        out.insert(it.key(), v.isString() ? QJsonValue(maskSecrets(QStringLiteral("%1: %2").arg(it.key(), v.toString())).mid(it.key().size() + 2))
                             : v.isObject() ? QJsonValue(maskedJson(v.toObject()))
                                            : v);
    }
    return out;
}
} // namespace

ToolResult AgentTools::httpRequestTool(const QJsonObject &a)
{
    if (!m_hooks.ide)
        return fail(QStringLiteral("the IDE cannot be controlled right now"));
    QString abs, err;
    if (!resolve(str(a, "path"), &abs, &err))
        return fail(err);
    if (!HttpFile::isHttpFile(abs))
        return fail(QStringLiteral("'%1' is not a .http or .rest file").arg(str(a, "path")));
    const QJsonObject started = m_hooks.ide(QStringLiteral("http_send"),
                                            QJsonObject{{"path", abs}, {"request", str(a, "request")}, {"line", a.value("line").toInt()},
                                                        {"environment", str(a, "environment")}});
    if (started.contains("error"))
        return fail(started.value("error").toString());
    QJsonObject r;
    const bool done = waitUntil([&] {
        r = m_hooks.ide(QStringLiteral("http_poll"), {});
        return !r.value("busy").toBool();
    }, kHttpWaitMs, m_cancelled);
    if (!done) {
        m_hooks.ide(QStringLiteral("http_stop"), {});
        return fail(m_cancelled ? QStringLiteral("cancelled by the user") : QStringLiteral("no response within %1 s").arg(kHttpWaitMs / 1000));
    }
    if (r.contains("error"))
        return fail(r.value("error").toString());
    // The response may echo credentials (a token in the body, a key in the url): mask every text in it like terminal output.
    QJsonObject response = maskedJson(r.value("response").toObject());
    if (!r.value("sent_first").toArray().isEmpty())
        response.insert("sent_first", r.value("sent_first"));
    if (response.contains("error"))
        return fail(QStringLiteral("%1: %2").arg(response.value("request").toString(), response.value("error").toString()));
    QString summary = QStringLiteral("%1 %2 · %3 ms").arg(response.value("status").toInt()).arg(response.value("reason").toString()).arg(response.value("ms").toInt());
    ToolResult out = success(response, summary.simplified());
    out.ok = response.value("status").toInt() < 400; // a 4xx / 5xx shows as failed in the chat; the model still gets the body
    return out;
}

void AgentTools::registerHttpTools()
{
    m_groupText.insert(QStringLiteral("http"),
                       QStringLiteral("HTTP requests\n"
                                      "- http_request sends a request written in a .http / .rest file (with its variables and environment, the "
                                      "requests it depends on first) and returns the response. Use it to check an endpoint after changing it; "
                                      "add a request to the project's .http file when there is none for it."));
    Spec http{"http_request", "Send one request of a .http / .rest file through QODE's HTTP client and return status, time, headers and "
                              "body (JSON indented, at most 12 KB; credentials masked). Pick the request by its # @name, its ### title or "
                              "its line. The user approves it.",
              makeSchema({{"path", "string", "The .http / .rest file."},
                          {"request", "string", "The request's # @name or ### title.", false},
                          {"line", "integer", "Or a line inside the request, 1-based.", false},
                          {"environment", "string", "Environment to use (from http-client.env.json and the like). Default: the one picked in "
                                                    "the HTTP panel. The choice stays.", false}}),
              Risk::Dangerous, [this](const QJsonObject &a) { return httpRequestTool(a); },
              [](const QJsonObject &a) {
                  const QString which = !str(a, "request").isEmpty() ? str(a, "request")
                                      : a.contains("line")            ? QStringLiteral("line %1").arg(a.value("line").toInt())
                                                                      : QStringLiteral("a request");
                  return QStringLiteral("Send %1 from %2").arg(which, str(a, "path"));
              },
              [this](const QJsonObject &a) {
                  // The request as written (variables not filled in, so no secret from an environment file shows).
                  QString abs, err, text;
                  if (!resolve(str(a, "path"), &abs, &err) || !m_hooks.readFile || !m_hooks.readFile(abs, &text))
                      return QStringLiteral("Send a request from %1").arg(str(a, "path"));
                  const HttpFileParse parse = HttpFile::parse(text);
                  int index = a.value("line").toInt() > 0 ? parse.requestAt(a.value("line").toInt() - 1) : -1;
                  for (int i = 0; i < parse.requests.size() && index < 0; ++i) {
                      const HttpRequestSpec &r = parse.requests.at(i);
                      if (!str(a, "request").isEmpty() && (r.name.compare(str(a, "request"), Qt::CaseInsensitive) == 0
                                                           || r.title.trimmed().compare(str(a, "request"), Qt::CaseInsensitive) == 0))
                          index = i;
                  }
                  if (index < 0)
                      return QStringLiteral("Send a request from %1").arg(str(a, "path"));
                  const HttpRequestSpec &r = parse.requests.at(index);
                  return QStringLiteral("Send from %1:\n\n%2 %3").arg(str(a, "path"), r.method, r.url);
              }};
    http.group = QStringLiteral("http");
    add(http);
}

} // namespace agent
