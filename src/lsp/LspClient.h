#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <functional>

class QProcess;

// A JSON-RPC 2.0 connection to one language server process over stdin/stdout (Content-Length framing).
// It performs the initialize handshake itself; ready() fires when the server accepts requests.
class LspClient : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Stopped };
    using Callback = std::function<void(const QJsonValue &result, const QJsonObject &error)>;

    LspClient(const QString &program, const QStringList &arguments, const QString &rootPath, const QJsonObject &initializationOptions = {},
              QObject *parent = nullptr);
    ~LspClient() override;

    // Adds a folder to the front of the server's PATH (so a Node.js script finds `node` when QODE was started from a
    // desktop launcher). Call before start().
    void prependToPath(const QString &dir);
    void start();
    // Sends shutdown + exit and waits briefly; kills the process when it does not leave on its own.
    void shutdown(int waitMs = 700);

    State state() const { return m_state; }
    bool isRunning() const { return m_state == State::Running; }
    QString serverName() const { return m_serverName; }
    QString serverVersion() const { return m_serverVersion; }
    QString errorString() const { return m_error; }
    QJsonObject serverCapabilities() const { return m_capabilities; }
    QStringList logLines() const { return m_log; } // the server's stderr, most recent last

    void logTiming(const QString &text); // adds a "[qode]" line to the server log
    int request(const QString &method, const QJsonValue &params, Callback callback = {});
    void notify(const QString &method, const QJsonValue &params);
    void cancel(int id);
    // Answers workspace/configuration: called once per requested item ({scopeUri, section}); advertised to the server
    // only when set. Without it every item is answered with null (server defaults).
    void setConfigurationProvider(std::function<QJsonValue(const QJsonObject &item)> provider) { m_configuration = std::move(provider); }
    // Answers the server's workspace/applyEdit (WorkspaceEdit in, true when every change was applied).
    void setApplyEditHandler(std::function<bool(const QJsonObject &edit)> handler) { m_applyEdit = std::move(handler); }

signals:
    void ready();
    void stopped(bool crashed);
    void notification(const QString &method, const QJsonValue &params);

private:
    void send(const QJsonObject &message);
    void onStdout();
    void handle(const QByteArray &body);
    void handleServerRequest(const QJsonObject &message);
    void onFinished(int exitCode, int exitStatus);
    void fail(const QString &error);

    QProcess *m_proc;
    QString m_root;
    QJsonObject m_initOptions, m_capabilities;
    State m_state = State::Idle;
    QByteArray m_buffer;
    bool m_parsing = false;
    int m_nextId = 1;
    QHash<int, Callback> m_pending;
    // Typing priority: background requests wait while an interactive one (completion, signature help, resolve) is
    // pending, so the server answers those first; held ones replace older duplicates and go out when it is free.
    struct Deferred { int id; QString method; QString key; QJsonValue params; Callback callback; };
    QList<Deferred> m_deferred;
    int m_interactive = 0; // pending interactive requests
    QSet<int> m_interactiveIds;
    void flushDeferred();
    void transmit(int id, const QString &method, const QJsonValue &params, Callback callback);
    struct Sent { QString method; QElapsedTimer timer; int outstanding = 0; };
    QHash<int, Sent> m_sent; // timing for the Show Server Log "[qode]" lines
    QString m_serverName, m_serverVersion, m_error;
    QStringList m_log;
    std::function<bool(const QJsonObject &)> m_applyEdit;
    std::function<QJsonValue(const QJsonObject &)> m_configuration;
    bool m_exiting = false; // we asked it to leave
};
