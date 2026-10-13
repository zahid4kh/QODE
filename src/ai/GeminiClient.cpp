#include "GeminiClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kMaxRetries = 4;
constexpr int kTimeoutMs = 180000;


// Seconds the server asks us to wait ("retryDelay": "12s" in the error details, or "retry in 12.3s" in the text).
int serverRetryDelay(const QJsonObject &errorBody)
{
    const QJsonObject err = errorBody.value(QStringLiteral("error")).toObject();
    for (const QJsonValue &d : err.value(QStringLiteral("details")).toArray()) {
        const QString delay = d.toObject().value(QStringLiteral("retryDelay")).toString();
        if (!delay.isEmpty())
            return int(delay.chopped(delay.endsWith(QLatin1Char('s')) ? 1 : 0).toDouble()) + 1;
    }
    const auto m = QRegularExpression(QStringLiteral("retry in ([0-9.]+)s")).match(err.value(QStringLiteral("message")).toString());
    return m.hasMatch() ? int(m.captured(1).toDouble()) + 1 : 0;
}

// What the user reads when a request finally fails.
QString messageOf(const QJsonObject &errorBody, int status, const QString &fallback, const QString &model)
{
    const QJsonObject err = errorBody.value(QStringLiteral("error")).toObject();
    QString msg = err.value(QStringLiteral("message")).toString();
    if (msg.isEmpty())
        msg = fallback;
    const QString state = err.value(QStringLiteral("status")).toString();
    const QString lower = msg.toLower();
    using T = QObject;
    if (status == 429 || state == QLatin1String("RESOURCE_EXHAUSTED")) {
        if (lower.contains(QLatin1String("perday")) || lower.contains(QLatin1String("per day")) || lower.contains(QLatin1String("daily")))
            return T::tr("The daily quota of the free tier is used up for this model. Pick another model or try again tomorrow.\n\n%1").arg(msg);
        return T::tr("Too many requests: the per-minute limit of the free tier was hit even after waiting. Give it a minute, then send again.\n\n%1").arg(msg);
    }
    if (status == 503 || state == QLatin1String("UNAVAILABLE"))
        return T::tr("The model is overloaded right now (\"%1\" is too busy). Try again in a moment or pick another model.\n\n%2").arg(model, msg);
    if (status == 500 || state == QLatin1String("INTERNAL"))
        return T::tr("Gemini had an internal error. Try again; if it keeps happening, pick another model.\n\n%1").arg(msg);
    if (status == 504 || state == QLatin1String("DEADLINE_EXCEEDED"))
        return T::tr("Gemini took too long to answer. Try a smaller request or another model.\n\n%1").arg(msg);
    if (status == 404 || state == QLatin1String("NOT_FOUND"))
        return T::tr("The model \"%1\" was not found or is not available for this key. Check the model name.\n\n%2").arg(model, msg);
    if (lower.contains(QLatin1String("api key")) || status == 401)
        return T::tr("The API key is invalid or was rejected. Click the key icon in the AI chat header to enter a new one.\n\n%1").arg(msg);
    if (status == 403 || state == QLatin1String("PERMISSION_DENIED"))
        return T::tr("Access denied (the key may lack permission for this model or region).\n\n%1").arg(msg);
    if (status == 400)
        return T::tr("Gemini rejected the request.\n\n%1").arg(msg);
    return T::tr("Gemini error %1: %2").arg(status).arg(msg);
}
} // namespace

GeminiClient::GeminiClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

QByteArray GeminiClient::buildBody(const Request &r)
{
    QJsonObject body;
    body.insert(QStringLiteral("contents"), r.contents);
    if (!r.systemInstruction.isEmpty())
        body.insert(QStringLiteral("systemInstruction"),
                    QJsonObject{{QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), r.systemInstruction}}}}});

    QJsonObject gen;
    if (!r.responseSchema.isEmpty()) {
        gen.insert(QStringLiteral("responseMimeType"), QStringLiteral("application/json"));
        gen.insert(QStringLiteral("responseSchema"), r.responseSchema);
    }
    gen.insert(QStringLiteral("thinkingConfig"), QJsonObject{{QStringLiteral("thinkingBudget"), r.thinkingBudget}});
    body.insert(QStringLiteral("generationConfig"), gen);

    // Safety filters off for all four adjustable categories.
    QJsonArray safety;
    for (const char *category : {"HARM_CATEGORY_HARASSMENT", "HARM_CATEGORY_HATE_SPEECH", "HARM_CATEGORY_SEXUALLY_EXPLICIT",
                                 "HARM_CATEGORY_DANGEROUS_CONTENT"})
        safety.append(QJsonObject{{QStringLiteral("category"), QLatin1String(category)}, {QStringLiteral("threshold"), QStringLiteral("OFF")}});
    body.insert(QStringLiteral("safetySettings"), safety);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void GeminiClient::generate(const Request &request, Callback callback)
{
    cancel();
    m_request = request;
    m_callback = std::move(callback);
    m_attempt = 0;
    m_cancelled = false;
    send();
}

void GeminiClient::cancel()
{
    if (!m_callback)
        return;
    m_cancelled = true;
    if (m_retryTimer) {
        m_retryTimer->stop();
        m_retryTimer->deleteLater();
        m_retryTimer = nullptr;
        Result r;
        r.error = tr("Cancelled");
        finish(r);
    } else if (m_reply) {
        m_reply->abort(); // finished() reports it
    }
}

void GeminiClient::send()
{
    QUrl url(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent").arg(m_request.model.trimmed()));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", m_request.apiKey.toUtf8());
    req.setTransferTimeout(kTimeoutMs);
    m_reply = m_net->post(req, buildBody(m_request));

    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply] {
        reply->deleteLater();
        if (reply != m_reply.data())
            return;
        m_reply = nullptr;

        Result r;
        r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        const QJsonObject json = QJsonDocument::fromJson(raw).object();

        if (m_cancelled || reply->error() == QNetworkReply::OperationCanceledError) {
            r.error = tr("Cancelled");
            finish(r);
            return;
        }
        if (reply->error() != QNetworkReply::NoError && r.httpStatus == 0) {
            if (m_attempt < kMaxRetries && reply->error() != QNetworkReply::TimeoutError) {
                ++m_attempt;
                const int wait = 3 * m_attempt;
                emit retrying(tr("Network problem (%1)").arg(reply->errorString()), wait);
                m_retryTimer = new QTimer(this);
                m_retryTimer->setSingleShot(true);
                connect(m_retryTimer, &QTimer::timeout, this, [this] {
                    m_retryTimer->deleteLater();
                    m_retryTimer = nullptr;
                    send();
                });
                m_retryTimer->start(wait * 1000);
                return;
            }
            r.error = reply->error() == QNetworkReply::TimeoutError
                ? tr("Gemini did not answer within %1 minutes. The model \"%2\" may be overloaded or unavailable; try another model.").arg(kTimeoutMs / 60000).arg(m_request.model)
                : tr("Network error: %1").arg(reply->errorString());
            finish(r);
            return;
        }
        if (r.httpStatus >= 400) {
            const bool transient = r.httpStatus == 429 || r.httpStatus == 500 || r.httpStatus == 503 || r.httpStatus == 504;
            // A spent daily quota will not come back within seconds: report it at once.
            const QString lowerMsg = json.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().toLower();
            const bool daily = r.httpStatus == 429 && (lowerMsg.contains(QLatin1String("perday")) || lowerMsg.contains(QLatin1String("per day")));
            if (transient && !daily && m_attempt < kMaxRetries) {
                ++m_attempt;
                int wait = 3 * m_attempt * m_attempt; // 3, 12, 27, 48 s unless the server names a delay
                if (const int asked = serverRetryDelay(json))
                    wait = asked;
                wait = qBound(2, wait, 60);
                const QString why = r.httpStatus == 429 ? tr("Rate limited")
                                    : r.httpStatus == 503 ? tr("Model is busy")
                                                          : tr("Server error %1").arg(r.httpStatus);
                emit retrying(tr("%1 (attempt %2 of %3)").arg(why).arg(m_attempt).arg(kMaxRetries), wait);
                m_retryTimer = new QTimer(this);
                m_retryTimer->setSingleShot(true);
                connect(m_retryTimer, &QTimer::timeout, this, [this] {
                    m_retryTimer->deleteLater();
                    m_retryTimer = nullptr;
                    send();
                });
                m_retryTimer->start(wait * 1000);
                return;
            }
            r.error = messageOf(json, r.httpStatus, reply->errorString(), m_request.model);
            finish(r);
            return;
        }

        const QJsonObject feedback = json.value(QStringLiteral("promptFeedback")).toObject();
        if (feedback.contains(QStringLiteral("blockReason"))) {
            r.error = tr("The prompt was blocked (%1).").arg(feedback.value(QStringLiteral("blockReason")).toString());
            finish(r);
            return;
        }
        const QJsonArray candidates = json.value(QStringLiteral("candidates")).toArray();
        if (candidates.isEmpty()) {
            r.error = tr("Gemini returned no answer.");
            finish(r);
            return;
        }
        const QJsonObject cand = candidates.first().toObject();
        r.finishReason = cand.value(QStringLiteral("finishReason")).toString();
        for (const QJsonValue &p : cand.value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray()) {
            const QJsonObject part = p.toObject();
            if (part.value(QStringLiteral("thought")).toBool())
                continue;
            r.text += part.value(QStringLiteral("text")).toString();
        }
        const QJsonObject usage = json.value(QStringLiteral("usageMetadata")).toObject();
        r.promptTokens = usage.value(QStringLiteral("promptTokenCount")).toInt();
        r.outputTokens = usage.value(QStringLiteral("candidatesTokenCount")).toInt();
        r.thoughtTokens = usage.value(QStringLiteral("thoughtsTokenCount")).toInt();

        if (r.finishReason == QLatin1String("MAX_TOKENS")) {
            r.error = tr("The answer was cut off (output limit reached). Try a smaller request.");
            finish(r);
            return;
        }
        if (r.text.trimmed().isEmpty()) {
            r.error = tr("Gemini returned an empty answer (finish reason: %1).").arg(r.finishReason.isEmpty() ? QStringLiteral("none") : r.finishReason);
            finish(r);
            return;
        }
        r.ok = true;
        finish(r);
    });
}

void GeminiClient::finish(const Result &result)
{
    Callback cb = std::move(m_callback);
    m_callback = nullptr;
    m_cancelled = false;
    if (cb)
        cb(result);
}
