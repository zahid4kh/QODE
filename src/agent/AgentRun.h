#pragma once

#include "agent/AgentTools.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QTimer;

namespace agent {

// One user request, carried out by Gemini with function calling: stream a model turn, run the tool calls it asks for,
// send the results back, repeat until the model answers without calling anything. Streams the thought summaries and the
// answer text as they arrive (`streamGenerateContent?alt=sse`). Tools run on the UI thread between two requests.
class AgentRun : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString apiKey;
        QString model;
        QString systemInstruction;
        QJsonArray contents;       // earlier turns + this request
        QString thinkingLevel = QStringLiteral("medium");
        int maxRounds = 40;        // model requests per run
    };

    // What the run cost (summed over its requests). `context` is the size of the last request's prompt.
    struct Usage {
        int requests = 0, prompt = 0, cached = 0, output = 0, context = 0;
    };

    AgentRun(AgentTools *tools, QObject *parent = nullptr);

    void start(const Config &config);
    void cancel();
    bool running() const { return m_running; }

    // The fixed instructions, then the project's AGENTS.md when there is one (the text stays the same between requests).
    static QString systemPrompt(const QString &root);
    static QString baseInstructions(const QString &root);

signals:
    void thoughtDelta(const QString &text);
    void textDelta(const QString &text);
    void toolStarted(const QString &name, const QString &call);
    void toolFinished(const QString &name, const agent::ToolResult &result);
    void status(const QString &text);        // "Thinking…", "Waiting 12 s (rate limited)…"; empty = back to normal
    void usage(const agent::AgentRun::Usage &usage); // before finished() / failed()
    void finished(int rounds);
    void failed(const QString &error);

private:
    void send();
    void onReadyRead();
    void onReplyFinished();
    void handleEvent(const QJsonObject &event);
    void addModelPart(const QJsonObject &part);
    bool runTools();
    void finish(const QString &error);
    void retryLater(const QString &why, int seconds);
    static QJsonArray pruneOldResults(const QJsonArray &contents);

    AgentTools *m_tools;
    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_retryTimer;
    Config m_cfg;
    QJsonArray m_contents;
    QJsonArray m_modelParts;   // the model turn being received, verbatim (thought signatures must travel back)
    QByteArray m_buffer, m_raw;
    QString m_inlineError;     // an error object / block reason inside the stream
    bool m_running = false;
    bool m_cancelled = false;
    bool m_inTools = false;
    bool m_gotOutput = false;  // this round already showed text: no silent retry
    bool m_anyText = false;    // the run produced visible text so far
    QString m_finishReason;
    Usage m_usage;
    Usage m_round;             // usageMetadata of the request being received
    int m_rounds = 0;
    int m_attempt = 0;
};

} // namespace agent

Q_DECLARE_METATYPE(agent::ToolResult)
Q_DECLARE_METATYPE(agent::AgentRun::Usage)
