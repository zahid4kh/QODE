#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <functional>

class QNetworkAccessManager;
class QTimer;

// Plain REST client for the Claude Messages API (`POST /v1/messages`); no SDK. Also the one place that knows how each
// Claude model wants to be asked (thinking, effort, output limit, beta features), shared with the streaming agent run.
// The non-streaming generate() is used for one-off calls such as /compact.
class ClaudeClient : public QObject
{
    Q_OBJECT
public:
    struct Request {
        QString apiKey;
        QString model;
        QString system;
        QJsonArray contents;           // Gemini-shaped history [{role: "user"|"model", parts: [{text}]}]; converted
        int maxTokens = 16000;
        QString effort = QStringLiteral("low");
    };
    struct Result {
        bool ok = false;
        QString text;
        QString error;
        QString stopReason;
        int promptTokens = 0, outputTokens = 0;
        int httpStatus = 0;
    };
    using Callback = std::function<void(const Result &)>;

    explicit ClaudeClient(QObject *parent = nullptr);

    // Sends the request; the callback runs once (also after cancel(), with ok = false and error "Cancelled").
    void generate(const Request &request, Callback callback);
    void cancel();
    bool busy() const;

    // --- shared with the streaming agent run ---------------------------------------------------------------------
    static QNetworkRequest makeRequest(const QString &apiKey, const QStringList &betas = {});
    // [{role, parts:[{text}]}] -> [{role: "user"|"assistant", content: text}] (same roles merged).
    static QJsonArray messagesFromContents(const QJsonArray &contents);
    // The `thinking` object for the model (adaptive with summarized display on current models, a token budget on the
    // older ones); empty = leave the field out.
    static QJsonObject thinkingFor(const QString &model);
    // The `output_config.effort` the model accepts closest to `wanted` (low|medium|high|xhigh|max); "" = leave it out.
    static QString effortFor(const QString &model, const QString &wanted);
    static int maxTokensFor(const QString &model);
    // Server-side refusal fallbacks (`fallbacks: "default"`) are offered for this model.
    static bool offersFallbacks(const QString &model);
    // The model binds thinking blocks to the conversation (preserved thinking), so `block_binding` is worth sending.
    static bool bindsThinking(const QString &model);
    // Seconds from the retry-after header, 0 when absent.
    static int retryAfter(const QNetworkReply *reply);
    static bool isTransient(int httpStatus);
    static QString errorText(const QJsonObject &errorBody, int httpStatus, const QString &fallback, const QString &model);

signals:
    void retrying(const QString &reason, int seconds);

private:
    void send();
    void finish(const Result &result);

    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_retryTimer;
    Request m_request;
    Callback m_callback;
    int m_attempt = 0;
    bool m_cancelled = false;
};
