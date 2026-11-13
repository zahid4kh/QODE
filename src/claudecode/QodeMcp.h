#pragma once

#include "agent/AgentTools.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace claudecode {

// QODE's own MCP server for the Claude Code CLI, named "qode" and hosted inside QODE: the CLI is told about it in the
// initialize request (Session::Options::sdkMcpServers) and sends its JSON-RPC (initialize, tools/list, tools/call) in
// `mcp_message` control requests; the answers go back as control responses. No process, port or config file.
//
// The tools are the QODE chat's IDE tools (problems and diagnostics, the servers QODE runs, the web preview, panels, the
// terminal, language servers), run by a second AgentTools whose hooks have no edits, saves or approvals: Claude Code
// edits files and runs commands itself, and it asks the user before a call that the QODE chat would ask about too.
// Calls run one at a time, in order, outside the read loop of the session (they wait in nested event loops).
class QodeMcp : public QObject
{
    Q_OBJECT
public:
    static QString serverName() { return QStringLiteral("qode"); }
    static QString toolPrefix() { return QStringLiteral("mcp__qode__"); } // how the CLI names the tools

    explicit QodeMcp(QObject *parent = nullptr);

    void setHooks(const agent::AgentTools::Hooks &hooks);
    void setRoot(const QString &root);
    bool isReady() const { return m_hasHooks && !m_root.isEmpty(); }
    bool cancelled() const { return m_tools.cancelled(); }

    // One mcp_message from the CLI; the answer comes as `reply` (at once for a notification).
    void handle(const QString &requestId, const QJsonObject &message);
    void cancel(const QString &requestId); // the CLI withdrew the request
    void reset();                          // a new CLI process: drop what is queued, stop what runs, answer nothing more
    // `cliToolName` (mcp__qode__x) runs without a permission card: the QODE chat runs x without asking too.
    bool runsWithoutAsking(const QString &cliToolName) const;
    // What QODE runs and shows now as an <ide_state> block (the QODE chat's editor-context lines: servers, the web
    // preview, problem counts, a terminal command); `quiet` is set when there is nothing to report. "" when not ready.
    QString stateBlock(bool *quiet = nullptr) const;

signals:
    void reply(const QString &requestId, const QJsonObject &message); // an empty message answers a notification

private:
    void next();
    void cancelCall(const QJsonValue &rpcId); // notifications/cancelled
    QJsonObject answer(const QJsonObject &message);
    QJsonObject callTool(const QString &name, const QJsonObject &args);
    void refreshGroups();
    QString instructions() const;
    static const QStringList &exposed();

    agent::AgentTools m_tools;
    bool m_hasHooks = false;
    QString m_root;
    QStringList m_groups; // the tool groups the project offers (AgentTools::setGroups), asked again on tools/list
    struct Pending {
        QString id;
        QJsonObject message;
    };
    QList<Pending> m_queue;
    QString m_running;        // the request being answered
    QJsonValue m_runningRpc;  // its JSON-RPC id
    bool m_dropRunning = false; // withdrawn or the process went away: its answer is not sent
};

} // namespace claudecode
