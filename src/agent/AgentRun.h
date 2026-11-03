#pragma once

#include "agent/AgentSession.h"
#include "agent/AgentTools.h"
#include "ai/AiEnv.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QVector>

class QNetworkAccessManager;
class QTimer;

namespace agent {

// One user request, carried out by Gemini or Claude with tool calling: stream a model turn, run the tool calls it asks
// for, send the results back, repeat until the model answers without calling anything. Streams the thought summaries
// and the answer text as they arrive (Gemini `streamGenerateContent?alt=sse`, Claude `POST /v1/messages` with
// `stream: true`; the Claude half lives in AgentRunClaude.cpp). Both use the same AgentTools. Tools run on the UI
// thread between two requests.
class AgentRun : public QObject
{
    Q_OBJECT
public:
    struct Config {
        AiEnv::Provider provider = AiEnv::Provider::Gemini;
        QString apiKey;
        QString model;
        QString systemInstruction;
        QJsonArray contents;       // earlier turns + this request, Gemini-shaped (converted for Claude)
        QString thinkingLevel = QStringLiteral("medium"); // Gemini
        QString effort = QStringLiteral("medium");        // Claude output_config.effort
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
    // The request being streamed, about four times a second and on every usage report, then once more with `done` set
    // (also for a stopped or failed request that had started; one that is retried is reported again from the start).
    void requestProgress(const agent::RequestStats &request);
    void finished(int rounds);
    void failed(const QString &error);

private:
    void send();
    void onReadyRead();
    void onReplyFinished();
    void handleEvent(const QJsonObject &event);
    void addModelPart(const QJsonObject &part);
    bool runTools();
    bool checkEdits(QVector<ToolResult> *results); // diagnostics after the round's edits
    void finish(const QString &error);
    void retryLater(const QString &why, int seconds);
    static QJsonArray pruneOldResults(const QJsonArray &contents);

    // Request timing and token estimates
    void beginRequest();
    void noteOutput(bool thought, int chars);   // streamed output of the current request
    void noteReportedOutput(int output, int thinking); // the server's cumulative count: the estimate starts over
    void closeThinking();
    void endRequest(bool stopped);
    RequestStats currentRequest() const;
    bool claude() const { return m_cfg.provider == AiEnv::Provider::Claude; }

    // Claude (AgentRunClaude.cpp)
    void sendClaude();
    void handleClaudeEvent(const QJsonObject &event);
    void claudeReplyFinished(QNetworkReply *reply);
    QJsonArray claudeContent(QVector<bool> *badInput) const;
    bool runClaudeTools(const QJsonArray &content, const QVector<bool> &badInput);
    void stripClaudeThinking();

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
    RequestStats m_round;      // usage reported for the request being received
    QElapsedTimer m_clock;     // since the request was sent
    QTimer *m_tick;
    bool m_inRequest = false;
    qint64 m_thinkFrom = -1;   // the open thinking stretch, ms
    int m_estChars = 0, m_estThoughtChars = 0; // streamed since the server last reported output tokens
    int m_rounds = 0;
    int m_attempt = 0;

    // Claude state. The history is append-only within a run: the newer models bind each thinking block to everything
    // before it, so nothing earlier is ever rewritten (no pruning of old tool results as on the Gemini side).
    QJsonArray m_messages;     // Messages API `messages`
    QVector<QJsonObject> m_blocks; // the assistant content being received, by stream index
    QHash<int, QString> m_partialJson; // tool_use input as it streams
    QSet<int> m_badInput;      // tool_use blocks whose input was not valid JSON
    int m_fallbackAt = -1;     // index of the last `fallback` block (a safety fallback switched models mid-answer)
    bool m_lastWasThought = false;
    bool m_strippedThinking = false;
    QString m_inlineErrorType;
    QString m_stopReason;
    QJsonObject m_stopDetails;
};

} // namespace agent

Q_DECLARE_METATYPE(agent::ToolResult)
Q_DECLARE_METATYPE(agent::AgentRun::Usage)
Q_DECLARE_METATYPE(agent::RequestStats)
