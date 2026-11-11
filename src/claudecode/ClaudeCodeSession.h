#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>

class QProcess;

namespace claudecode {

// One running Claude Code conversation: `claude -p` with stream-json in and out, kept alive across turns. Every line it
// prints is a JSON message (system init, stream events, assistant / user messages, result, rate limits); permission
// prompts arrive as `control_request` can_use_tool (--permission-prompt-tool stdio) and are answered with a
// `control_response`. QODE's own requests (initialize, interrupt, set_model, set_permission_mode) are control requests
// too. The protocol is the one the Agent SDK speaks; see dev-docs/claude-code.md.
class Session : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QString root;            // the working directory: the project
        QString model;           // "" = the account's default; an alias (opus, sonnet, …) or a model id
        QString permissionMode;  // default | acceptEdits | plan | bypassPermissions
        QString resume;          // a session id to continue
    };

    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    bool isRunning() const;
    // A process that was stopped and is still saving holds the next start back until it has exited, so `--resume`
    // never reads a conversation another process is still writing. What is sent meanwhile waits.
    void start(const Options &options);
    // Ends the process without waiting for it (the conversation stays on disk and can be resumed): input is closed,
    // so the CLI saves and exits; it is terminated after 1.5 s and killed 1 s later if it is still running.
    void stop();
    QString sessionId() const { return m_sessionId; }

    void sendUser(const QJsonValue &content); // a string, or content blocks (text, images)
    void interrupt();
    void setModel(const QString &model);
    void setPermissionMode(const QString &mode);
    // Any other control request ({"subtype":"get_usage"}, mcp_status, …): its answer comes as `answered` with the
    // returned id. Requests that need no model call, so they cost nothing.
    QString query(const QJsonObject &request);
    // The answer to a can_use_tool request: {"behavior":"allow","updatedInput":…} or {"behavior":"deny","message":…}.
    void answerPermission(const QString &requestId, const QJsonObject &decision);

signals:
    void initialized(const QJsonObject &info);       // the initialize response: commands, models, account, …
    void messageReceived(const QJsonObject &message);
    void permissionRequested(const QString &requestId, const QJsonObject &request);
    void permissionCancelled(const QString &requestId);
    void answered(const QString &requestId, const QJsonObject &response, const QString &error); // error: "" on success
    void exited(int exitCode, const QString &errorText); // errorText: the end of stderr when it did not end cleanly

private:
    void write(const QJsonObject &o);
    void request(const QJsonObject &request, const QString &id = QString());
    void readOut();
    void handle(const QJsonObject &o);
    void startPending();
    static void endNow(QProcess *p); // the blocking stop, for QODE's exit

    QProcess *m_proc = nullptr;
    QList<QProcess *> m_closing; // stopped, still exiting
    QByteArray m_buffer;
    QByteArray m_stderr;
    QList<QByteArray> m_pending; // written before the process was up
    QString m_sessionId;
    QString m_initId;
    int m_nextId = 0;
    bool m_stopping = false;
};

} // namespace claudecode
