#include "ClaudeClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kMaxAttempts = 3;

// What a model id says about the model. Ids look like "claude-opus-5-5", "claude-haiku-4-5-20251001", "claude-fable-5";
// anything that does not parse (a future naming scheme, "claude-mythos-preview") counts as a current model.
struct Family {
    QString line;       // opus | sonnet | haiku | fable | mythos, "" when unknown
    int version = 999;  // major * 10 + minor: 55 = 5.5, 45 = 4.5
    bool legacy3 = false; // claude-3-* naming
};

Family familyOf(const QString &model)
{
    Family f;
    const QString m = model.trimmed().toLower();
    if (m.startsWith(QLatin1String("claude-3"))) {
        f.legacy3 = true;
        f.version = 37;
        return f;
    }
    static const QRegularExpression re(QStringLiteral("^claude-(opus|sonnet|haiku|fable|mythos)-(\\d+)(?:-(\\d+))?"));
    const QRegularExpressionMatch mt = re.match(m);
    if (!mt.hasMatch())
        return f;
    f.line = mt.captured(1);
    const int major = mt.captured(2).toInt();
    const int minor = mt.captured(3).isEmpty() || mt.captured(3).size() > 2 ? 0 : mt.captured(3).toInt(); // a date suffix is no minor
    f.version = major * 10 + minor;
    return f;
}

bool frontier(const Family &f)
{
    return f.line == QLatin1String("fable") || f.line == QLatin1String("mythos");
}

} // namespace

ClaudeClient::ClaudeClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_retryTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &ClaudeClient::send);
}

// --- model knowledge ------------------------------------------------------------------------------------------------

QNetworkRequest ClaudeClient::makeRequest(const QString &apiKey, const QStringList &betas)
{
    // ANTHROPIC_BASE_URL (as in the official SDKs) points the client at a proxy or a local test server.
    QString base = qEnvironmentVariable("ANTHROPIC_BASE_URL").trimmed();
    if (base.isEmpty())
        base = QStringLiteral("https://api.anthropic.com");
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QNetworkRequest req{QUrl(base + QStringLiteral("/v1/messages"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-api-key", apiKey.trimmed().toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");
    if (!betas.isEmpty())
        req.setRawHeader("anthropic-beta", betas.join(QLatin1Char(',')).toUtf8());
    return req;
}

QJsonArray ClaudeClient::messagesFromContents(const QJsonArray &contents)
{
    QJsonArray out;
    QString role, text;
    auto flush = [&] {
        if (!role.isEmpty() && !text.trimmed().isEmpty())
            out.append(QJsonObject{{"role", role}, {"content", text}});
    };
    for (const QJsonValue &v : contents) {
        const QJsonObject turn = v.toObject();
        const QString r = turn.value("role").toString() == QLatin1String("model") ? QStringLiteral("assistant") : QStringLiteral("user");
        QStringList parts;
        for (const QJsonValue &p : turn.value("parts").toArray())
            if (p.toObject().contains("text"))
                parts << p.toObject().value("text").toString();
        const QString t = parts.join(QStringLiteral("\n\n"));
        if (r == role) {
            text += QStringLiteral("\n\n") + t;
        } else {
            flush();
            role = r;
            text = t;
        }
    }
    flush();
    return out;
}

QJsonObject ClaudeClient::thinkingFor(const QString &model)
{
    const Family f = familyOf(model);
    if (f.legacy3)
        return {};
    // 4.6 and later (and Fable / Mythos) think adaptively; the summary is asked for explicitly because newer models
    // default to an empty ("omitted") thinking text, which would leave the transcript silent while the agent works.
    if (frontier(f) || f.version >= 46)
        return QJsonObject{{"type", "adaptive"}, {"display", "summarized"}};
    return QJsonObject{{"type", "enabled"}, {"budget_tokens", 8000}};
}

QString ClaudeClient::effortFor(const QString &model, const QString &wanted)
{
    const Family f = familyOf(model);
    QString e = wanted.trimmed().toLower();
    static const QStringList all = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")};
    if (!all.contains(e))
        e = QStringLiteral("high");
    if (f.legacy3)
        return {};
    if (frontier(f) || f.version >= 47)
        return e;
    if (f.version == 46) // no xhigh before 4.7
        return e == QLatin1String("xhigh") ? QStringLiteral("high") : e;
    if (f.line == QLatin1String("opus") && f.version == 45) // low / medium / high only
        return e == QLatin1String("xhigh") || e == QLatin1String("max") ? QStringLiteral("high") : e;
    return {}; // Sonnet 4.5, Haiku 4.5 and older reject the field
}

int ClaudeClient::maxTokensFor(const QString &model)
{
    const Family f = familyOf(model);
    if (f.legacy3)
        return 8192;
    if (f.line == QLatin1String("opus") && f.version < 45)
        return 32000;
    return 64000; // room for thinking plus a long reply; the agent always streams
}

bool ClaudeClient::offersFallbacks(const QString &model)
{
    const Family f = familyOf(model);
    if (f.line == QLatin1String("fable"))
        return true;
    if (f.line == QLatin1String("opus"))
        return f.version >= 50 && f.version < 999;
    if (f.line == QLatin1String("sonnet"))
        return f.version >= 55 && f.version < 999;
    return false; // Haiku has no server-side fallback; unknown ids are not guessed
}

bool ClaudeClient::bindsThinking(const QString &model)
{
    const Family f = familyOf(model);
    if (f.line == QLatin1String("fable"))
        return f.version >= 51;
    if (f.line == QLatin1String("opus") || f.line == QLatin1String("sonnet") || f.line == QLatin1String("haiku"))
        return f.version >= 55 && f.version < 999;
    return false;
}

int ClaudeClient::retryAfter(const QNetworkReply *reply)
{
    if (!reply || !reply->hasRawHeader("retry-after"))
        return 0;
    bool ok = false;
    const double s = QString::fromLatin1(reply->rawHeader("retry-after")).trimmed().toDouble(&ok);
    return ok && s > 0 ? qMax(1, int(s + 0.999)) : 0;
}

bool ClaudeClient::isTransient(int httpStatus)
{
    return httpStatus == 429 || httpStatus == 500 || httpStatus == 502 || httpStatus == 503 || httpStatus == 504 || httpStatus == 529;
}

QString ClaudeClient::errorText(const QJsonObject &errorBody, int httpStatus, const QString &fallback, const QString &model)
{
    const QJsonObject err = errorBody.value("error").toObject();
    const QString message = err.value("message").toString().trimmed();
    const QString detail = message.isEmpty() ? fallback : message;
    switch (httpStatus) {
    case 401:
        return tr("Anthropic rejected the API key. Check it in the Claude API key dialog (%1).").arg(detail);
    case 402:
        return tr("Anthropic reports a billing problem with this account: %1").arg(detail);
    case 403:
        return tr("This API key may not use \"%1\": %2").arg(model, detail);
    case 404:
        return tr("The model \"%1\" was not found or this key cannot use it: %2").arg(model, detail);
    case 413:
        return tr("The request is too large for the Claude API. Start a new chat or use /compact.");
    case 429:
        return tr("Claude rate limit or spend limit reached: %1").arg(detail);
    case 529:
        return tr("Claude is overloaded right now. Try again in a moment or pick another model.");
    default:
        break;
    }
    if (httpStatus >= 500)
        return tr("The Claude API had a server error (%1): %2").arg(httpStatus).arg(detail);
    return httpStatus > 0 ? tr("Claude API error %1: %2").arg(httpStatus).arg(detail) : tr("Network error: %1").arg(detail);
}

// --- one-off requests -----------------------------------------------------------------------------------------------

void ClaudeClient::generate(const Request &request, Callback callback)
{
    cancel();
    m_request = request;
    m_callback = std::move(callback);
    m_attempt = 0;
    m_cancelled = false;
    send();
}

bool ClaudeClient::busy() const
{
    return m_reply != nullptr || m_retryTimer->isActive();
}

void ClaudeClient::cancel()
{
    m_retryTimer->stop();
    if (!m_callback)
        return;
    m_cancelled = true;
    if (m_reply) {
        m_reply->abort(); // finished() reports it
        return;
    }
    Result r;
    r.error = QStringLiteral("Cancelled");
    finish(r);
}

void ClaudeClient::finish(const Result &result)
{
    Callback cb = std::move(m_callback);
    m_callback = nullptr;
    m_reply = nullptr;
    if (cb)
        cb(result);
}

void ClaudeClient::send()
{
    QJsonObject body{{"model", m_request.model.trimmed()},
                     {"max_tokens", m_request.maxTokens},
                     {"messages", messagesFromContents(m_request.contents)}};
    if (!m_request.system.isEmpty())
        body.insert("system", m_request.system);
    const QString effort = effortFor(m_request.model, m_request.effort);
    if (!effort.isEmpty())
        body.insert("output_config", QJsonObject{{"effort", effort}});

    QNetworkRequest req = makeRequest(m_request.apiKey);
    req.setTransferTimeout(180000);
    m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply] {
        reply->deleteLater();
        if (reply != m_reply)
            return;
        m_reply = nullptr;
        Result r;
        r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        if (m_cancelled || reply->error() == QNetworkReply::OperationCanceledError) {
            r.error = QStringLiteral("Cancelled");
            finish(r);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || r.httpStatus >= 400) {
            const bool transient = r.httpStatus == 0 || (isTransient(r.httpStatus) && (r.httpStatus != 429 || retryAfter(reply) > 0));
            if (transient && m_attempt + 1 < kMaxAttempts) {
                ++m_attempt;
                const int wait = qBound(2, retryAfter(reply) ? retryAfter(reply) : 3 * m_attempt * m_attempt, 60);
                emit retrying(r.httpStatus == 429 ? tr("Rate limited") : tr("Claude is busy"), wait);
                m_retryTimer->start(wait * 1000);
                return;
            }
            r.error = errorText(o, r.httpStatus, reply->errorString(), m_request.model);
            finish(r);
            return;
        }
        r.stopReason = o.value("stop_reason").toString();
        const QJsonObject usage = o.value("usage").toObject();
        r.promptTokens = usage.value("input_tokens").toInt() + usage.value("cache_read_input_tokens").toInt()
                         + usage.value("cache_creation_input_tokens").toInt();
        r.outputTokens = usage.value("output_tokens").toInt();
        QStringList parts;
        for (const QJsonValue &v : o.value("content").toArray())
            if (v.toObject().value("type").toString() == QLatin1String("text"))
                parts << v.toObject().value("text").toString();
        r.text = parts.join(QString());
        if (r.stopReason == QLatin1String("refusal")) {
            r.error = tr("Claude declined this request.");
        } else if (r.text.trimmed().isEmpty()) {
            r.error = tr("Claude returned an empty answer (stop reason: %1).").arg(r.stopReason.isEmpty() ? QStringLiteral("none") : r.stopReason);
        } else {
            r.ok = true;
        }
        finish(r);
    });
}
