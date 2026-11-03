#include "InlineClient.h"

#include "ai/AiEnv.h"
#include "ai/ClaudeClient.h"
#include "ai/GeminiClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kTimeoutMs = 120000;
constexpr int kMaxAttempts = 2;
}

InlineClient::InlineClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool InlineClient::claude() const
{
    return AiEnv::providerForModel(m_model) == AiEnv::Provider::Claude;
}

void InlineClient::start(const QString &apiKey, const QString &model, const QString &system, const QJsonArray &contents)
{
    cancel();
    m_key = apiKey.trimmed();
    m_model = model.trimmed();
    m_system = system;
    m_contents = contents;
    m_cancelled = false;
    m_attempt = 0;
    send();
}

void InlineClient::cancel()
{
    const bool running = busy();
    m_cancelled = true;
    if (m_retry) {
        delete m_retry;
        m_retry = nullptr;
    }
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    if (running)
        emit finished(false, tr("Stopped."), {});
}

void InlineClient::send()
{
    m_buffer.clear();
    m_raw.clear();
    m_inlineError.clear();
    m_stop.clear();
    m_gotText = false;
    QNetworkRequest req;
    QJsonObject body;
    if (claude()) {
        body = QJsonObject{{"model", m_model},
                           {"max_tokens", 16000},
                           {"stream", true},
                           {"system", m_system},
                           {"messages", ClaudeClient::messagesFromContents(m_contents)}};
        // No thinking: an inline edit wants its first line quickly. Effort keeps the answer short where it is understood.
        const QString effort = ClaudeClient::effortFor(m_model, QStringLiteral("low"));
        if (!effort.isEmpty())
            body.insert("output_config", QJsonObject{{"effort", effort}});
        req = ClaudeClient::makeRequest(m_key);
    } else {
        body = QJsonObject{{"systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", m_system}}}}}},
                           {"contents", m_contents},
                           {"generationConfig", QJsonObject{{"thinkingConfig", QJsonObject{{"thinkingLevel", "low"}}}}}};
        req = QNetworkRequest(QUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:streamGenerateContent?alt=sse").arg(m_model)));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("x-goog-api-key", m_key.toUtf8());
    }
    req.setTransferTimeout(kTimeoutMs);
    m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &InlineClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &InlineClient::onFinished);
}

void InlineClient::onReadyRead()
{
    if (!m_reply)
        return;
    const QByteArray chunk = m_reply->readAll();
    if (m_raw.size() < 256 * 1024)
        m_raw += chunk;
    m_buffer += chunk;
    int nl;
    while ((nl = m_buffer.indexOf('\n')) != -1) {
        QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (!line.startsWith("data:"))
            continue; // an error body is read from m_raw at the end
        line = line.mid(5).trimmed();
        if (line.isEmpty() || line == "[DONE]")
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            handleEvent(doc.object());
    }
}

void InlineClient::handleEvent(const QJsonObject &event)
{
    auto text = [this](const QString &t) {
        if (t.isEmpty())
            return;
        m_gotText = true;
        emit textDelta(t);
    };
    if (claude()) {
        const QString type = event.value("type").toString();
        if (type == QLatin1String("content_block_start")) {
            const QJsonObject block = event.value("content_block").toObject();
            if (block.value("type").toString() == QLatin1String("text"))
                text(block.value("text").toString());
        } else if (type == QLatin1String("content_block_delta")) {
            const QJsonObject delta = event.value("delta").toObject();
            if (delta.value("type").toString() == QLatin1String("text_delta"))
                text(delta.value("text").toString());
        } else if (type == QLatin1String("message_delta")) {
            const QJsonValue reason = event.value("delta").toObject().value("stop_reason");
            if (reason.isString())
                m_stop = reason.toString();
        } else if (type == QLatin1String("error")) {
            m_inlineError = event.value("error").toObject().value("message").toString(tr("The model reported an error."));
        }
        return;
    }
    if (event.value("error").isObject()) {
        m_inlineError = event.value("error").toObject().value("message").toString(tr("The model reported an error."));
        return;
    }
    const QString blocked = event.value("promptFeedback").toObject().value("blockReason").toString();
    if (!blocked.isEmpty())
        m_inlineError = tr("The request was blocked (%1).").arg(blocked);
    const QJsonArray candidates = event.value("candidates").toArray();
    if (candidates.isEmpty())
        return;
    const QJsonObject cand = candidates.first().toObject();
    const QString reason = cand.value("finishReason").toString();
    if (!reason.isEmpty())
        m_stop = reason;
    for (const QJsonValue &v : cand.value("content").toObject().value("parts").toArray()) {
        const QJsonObject part = v.toObject();
        if (!part.value("thought").toBool())
            text(part.value("text").toString());
    }
}

void InlineClient::onFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    onReadyRead();
    m_reply = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError net = reply->error();
    if (m_cancelled || net == QNetworkReply::OperationCanceledError)
        return;
    if (net != QNetworkReply::NoError || status >= 400) {
        const QJsonObject body = QJsonDocument::fromJson(m_raw).object();
        const bool transient = status == 0 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504 || status == 529;
        if (!m_gotText && transient && net != QNetworkReply::TimeoutError && m_attempt < kMaxAttempts) {
            ++m_attempt;
            const int wait = 2 * m_attempt;
            emit retrying(status == 429 ? tr("Rate limited") : status == 0 ? tr("Network problem") : tr("Server busy"), wait);
            m_retry = new QTimer(this);
            m_retry->setSingleShot(true);
            connect(m_retry, &QTimer::timeout, this, [this] {
                m_retry->deleteLater();
                m_retry = nullptr;
                send();
            });
            m_retry->start(wait * 1000);
            return;
        }
        if (status == 0)
            done(false, net == QNetworkReply::TimeoutError ? tr("The model did not answer in time.") : tr("Network error: %1").arg(reply->errorString()));
        else if (claude())
            done(false, ClaudeClient::errorText(body, status, reply->errorString(), m_model));
        else
            done(false, GeminiClient::errorText(body, status, reply->errorString(), m_model));
        return;
    }
    if (!m_inlineError.isEmpty()) {
        done(false, m_inlineError);
        return;
    }
    if (m_stop == QLatin1String("refusal") || m_stop == QLatin1String("SAFETY") || m_stop == QLatin1String("PROHIBITED_CONTENT")) {
        done(false, tr("The model declined this request."));
        return;
    }
    const bool cut = m_stop == QLatin1String("max_tokens") || m_stop == QLatin1String("MAX_TOKENS");
    done(true, {}, cut ? tr("The answer reached the output limit and may be incomplete.") : QString());
}

void InlineClient::done(bool ok, const QString &error, const QString &note)
{
    emit finished(ok, error, note);
}
