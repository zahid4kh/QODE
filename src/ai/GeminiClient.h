#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <functional>

class QNetworkAccessManager;
class QTimer;

// Plain REST client for the Gemini API (`models/<model>:generateContent`); no SDK. Every call is a stateless request: the
// caller passes the whole conversation in `contents` and the session's system prompt in `systemInstruction`.
class GeminiClient : public QObject
{
    Q_OBJECT
public:
    struct Request {
        QString apiKey;
        QString model;
        QString systemInstruction;   // sent once per request as the top-level field, never as a message
        QJsonArray contents;         // [{role: "user"|"model", parts: [{text}]}]
        QJsonObject responseSchema;  // non-empty => the answer is JSON of this shape
        int thinkingBudget = 1024;   // thinking is always on
    };
    struct Result {
        bool ok = false;
        QString text;          // concatenated answer parts (thoughts excluded)
        QString error;         // human readable when !ok
        QString finishReason;
        int promptTokens = 0, outputTokens = 0, thoughtTokens = 0;
        int httpStatus = 0;
    };
    using Callback = std::function<void(const Result &)>;

    explicit GeminiClient(QObject *parent = nullptr);

    // Sends the request; the callback runs once (also after cancel(), with ok = false and error "Cancelled").
    void generate(const Request &request, Callback callback);
    void cancel();
    bool busy() const { return m_reply != nullptr || m_retryTimer; }

    static QByteArray buildBody(const Request &request); // exposed for tests
    // Shared with the streaming agent client: seconds the server asks us to wait, and the message shown for a failed request.
    static int retryDelayOf(const QJsonObject &errorBody);
    static QString errorText(const QJsonObject &errorBody, int status, const QString &fallback, const QString &model);

signals:
    void retrying(const QString &reason, int seconds);

private:
    void send();
    void finish(const Result &result);

    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_retryTimer = nullptr;
    Request m_request;
    Callback m_callback;
    int m_attempt = 0;
    bool m_cancelled = false;
};
