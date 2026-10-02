#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
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

    void start();
    // Sends shutdown + exit and waits briefly; kills the process when it does not leave on its own.
    void shutdown(int waitMs = 700);

    State state() const { return m_state; }
    bool isRunning() const { return m_state == State::Running; }
    QString serverName() const { return m_serverName; }
    QString serverVersion() const { return m_serverVersion; }
    QString errorString() const { return m_error; }
    QStringList logLines() const { return m_log; } // the server's stderr, most recent last

    int request(const QString &method, const QJsonValue &params, Callback callback = {});
    void notify(const QString &method, const QJsonValue &params);
    void cancel(int id);

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
    QJsonObject m_initOptions;
    State m_state = State::Idle;
    QByteArray m_buffer;
    int m_nextId = 1;
    QHash<int, Callback> m_pending;
    QString m_serverName, m_serverVersion, m_error;
    QStringList m_log;
    bool m_exiting = false; // we asked it to leave
};
