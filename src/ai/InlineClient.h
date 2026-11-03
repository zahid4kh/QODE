#pragma once

#include <QJsonArray>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QTimer;

// One streamed request without tools, for the inline edit box: Gemini (`streamGenerateContent?alt=sse`) or Claude
// (`POST /v1/messages` with `stream`), picked by the model id. Thinking is kept low so the first text comes quickly.
// Transient failures (429 / 5xx / network) are retried twice while nothing has been streamed yet.
class InlineClient : public QObject
{
    Q_OBJECT
public:
    explicit InlineClient(QObject *parent = nullptr);

    // `contents` is Gemini-shaped: [{role: "user"|"model", parts: [{text}]}].
    void start(const QString &apiKey, const QString &model, const QString &system, const QJsonArray &contents);
    void cancel(); // emits finished(false, "Stopped.")
    bool busy() const { return m_reply || m_retry; }

signals:
    void textDelta(const QString &text);
    void retrying(const QString &reason, int seconds);
    // `error` is empty when ok; `note` tells about a cut-short answer (output limit).
    void finished(bool ok, const QString &error, const QString &note);

private:
    void send();
    void onReadyRead();
    void onFinished();
    void handleEvent(const QJsonObject &event);
    void done(bool ok, const QString &error, const QString &note = {});
    bool claude() const;

    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_retry = nullptr;
    QString m_key, m_model, m_system;
    QJsonArray m_contents;
    QByteArray m_buffer, m_raw;
    QString m_inlineError, m_stop;
    bool m_gotText = false;
    bool m_cancelled = false;
    int m_attempt = 0;
};
