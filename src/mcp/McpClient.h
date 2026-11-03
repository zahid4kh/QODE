#pragma once

#include "mcp/McpConfig.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>

class QProcess;
class QTimer;

namespace mcp {

// One MCP server over stdio: newline-delimited JSON-RPC 2.0 on the program's stdin / stdout (stderr is its log).
// start() runs `initialize`, `notifications/initialized` and `tools/list`; the server is Connected once the tools are
// known. A `notifications/tools/list_changed` lists them again.
class Client : public QObject
{
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Connected, Failed };

    Client(const ServerConfig &config, const QString &projectRoot, QObject *parent = nullptr);
    ~Client() override;

    void start();
    void stop();

    const ServerConfig &config() const { return m_config; }
    void setConfig(const ServerConfig &config) { m_config = config; } // the parts that need no restart (enabled, trusted)
    State state() const { return m_state; }
    QString error() const { return m_error; }
    QJsonArray tools() const { return m_tools; }        // MCP Tool objects: {name, description, inputSchema, annotations}
    QString instructions() const { return m_instructions; }
    QString serverTitle() const { return m_serverTitle; } // "Blender MCP 1.2" from serverInfo
    QString log() const { return QString::fromUtf8(m_stderr); }

    // Calls a tool and waits for the answer: the agent's tools run one after another on the UI thread (events keep
    // being processed, as for run_command). `cancelled` is polled; a cancelled call is announced to the server.
    // Returns the CallToolResult, or an empty object with `error` set.
    QJsonObject callTool(const QString &tool, const QJsonObject &args, int timeoutMs, const std::function<bool()> &cancelled, QString *error);

signals:
    void changed(); // state, error or tools

private:
    using Callback = std::function<void(const QJsonObject &result, const QString &error)>;
    int request(const QString &method, const QJsonObject &params, Callback done);
    void notify(const QString &method, const QJsonObject &params = {});
    void send(const QJsonObject &message);
    void readOut();
    void handle(const QJsonObject &message);
    void listTools(const QString &cursor = QString());
    void fail(const QString &error);
    void setState(State state);
    void failPending(const QString &error);

    ServerConfig m_config;
    QString m_root;
    QProcess *m_proc = nullptr;
    QTimer *m_startTimer;
    QByteArray m_buffer, m_stderr;
    QList<QByteArray> m_queued; // written before the process was running
    QHash<int, Callback> m_pending;
    int m_nextId = 0;
    int m_generation = 0; // callbacks of an earlier start are dropped
    State m_state = State::Stopped;
    QString m_error, m_instructions, m_serverTitle;
    QJsonArray m_tools, m_collect;
};

} // namespace mcp
