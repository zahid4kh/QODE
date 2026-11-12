#include "QodeMcp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

namespace claudecode {

namespace {
// The MCP revision answered when the CLI asks for none (it always names one).
constexpr const char *kProtocol = "2025-06-18";

QJsonObject rpcResult(const QJsonValue &id, const QJsonObject &result)
{
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QJsonObject rpcError(const QJsonValue &id, int code, const QString &message)
{
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
} // namespace

// What Claude Code gets. Not the file, edit, command or git tools (it has its own), not editor_command (it changes
// unsaved editor buffers while Claude Code writes the files on disk), and not yet the HTTP and debugger tools.
const QStringList &QodeMcp::exposed()
{
    static const QStringList names{
        // core
        "get_ide_state", "get_problems", "get_diagnostics", "semantic_search", "show_panel", "open_file", "terminal_output",
        "check_language_servers", "install_language_server",
        // group "servers"
        "server_status", "server_start", "server_restart", "server_stop", "server_log",
        // group "preview"
        "preview_status", "preview_screenshot", "preview_console", "preview_set_viewport", "preview_navigate", "preview_click",
        "preview_type", "preview_eval"};
    return names;
}

QodeMcp::QodeMcp(QObject *parent)
    : QObject(parent)
{
}

void QodeMcp::setHooks(const agent::AgentTools::Hooks &hooks)
{
    m_tools.setHooks(hooks);
    m_hasHooks = true;
}

void QodeMcp::setRoot(const QString &root)
{
    m_root = root;
    if (!root.isEmpty())
        m_tools.setRoot(root);
}

void QodeMcp::refreshGroups()
{
    m_groups.clear();
    if (m_tools.hooks().ide)
        for (const QJsonValue &v : m_tools.hooks().ide(QStringLiteral("tool_groups"), {}).value("groups").toArray())
            m_groups << v.toString();
    m_tools.setGroups(m_groups);
}

bool QodeMcp::runsWithoutAsking(const QString &cliToolName) const
{
    if (!cliToolName.startsWith(toolPrefix()))
        return false;
    const QString name = cliToolName.mid(toolPrefix().size());
    return exposed().contains(name) && !m_tools.asksFirst(name);
}

QString QodeMcp::stateBlock(bool *quiet) const
{
    if (quiet)
        *quiet = true;
    if (!isReady() || !m_tools.hooks().ide)
        return {};
    QStringList lines;
    for (const QJsonValue &v : m_tools.hooks().ide(QStringLiteral("summary"), {}).value("lines").toArray())
        lines << QStringLiteral("- ") + v.toString();
    if (quiet)
        *quiet = lines.isEmpty();
    if (lines.isEmpty())
        lines << QStringLiteral("- QODE runs no servers, reports no problems and no command is running in its terminal.");
    return QStringLiteral("<ide_state>What QODE runs and shows now (sent when it changes since your last message; the qode "
                          "tools have the details):\n%1</ide_state>").arg(lines.join(QLatin1Char('\n')));
}

void QodeMcp::handle(const QString &requestId, const QJsonObject &message)
{
    if (!message.contains("id")) { // a notification (initialized, cancelled): nothing to answer but the receipt
        emit reply(requestId, QJsonObject());
        return;
    }
    m_queue.append({requestId, message});
    if (m_running.isEmpty())
        QTimer::singleShot(0, this, &QodeMcp::next);
}

void QodeMcp::cancel(const QString &requestId)
{
    if (requestId == m_running) {
        m_dropRunning = true;
        m_tools.cancel();
        return;
    }
    for (int i = 0; i < m_queue.size(); ++i)
        if (m_queue.at(i).id == requestId) {
            m_queue.removeAt(i);
            return;
        }
}

void QodeMcp::reset()
{
    m_queue.clear();
    if (!m_running.isEmpty()) {
        m_dropRunning = true;
        m_tools.cancel();
    }
}

void QodeMcp::next()
{
    if (!m_running.isEmpty() || m_queue.isEmpty())
        return;
    const Pending p = m_queue.takeFirst();
    m_running = p.id;
    m_dropRunning = false;
    const QJsonObject out = answer(p.message);
    const bool drop = m_dropRunning;
    m_running.clear();
    m_dropRunning = false;
    if (!drop)
        emit reply(p.id, out);
    if (!m_queue.isEmpty())
        QTimer::singleShot(0, this, &QodeMcp::next);
}

QJsonObject QodeMcp::answer(const QJsonObject &message)
{
    const QJsonValue id = message.value("id");
    const QString method = message.value("method").toString();
    const QJsonObject params = message.value("params").toObject();
    if (method == QLatin1String("initialize")) {
        refreshGroups();
        return rpcResult(id, QJsonObject{{"protocolVersion", params.value("protocolVersion").toString(QString::fromLatin1(kProtocol))},
                                         {"capabilities", QJsonObject{{"tools", QJsonObject()}}},
                                         {"serverInfo", QJsonObject{{"name", serverName()}, {"version", QStringLiteral(QODE_VERSION)}}},
                                         {"instructions", instructions()}});
    }
    if (method == QLatin1String("ping"))
        return rpcResult(id, QJsonObject());
    if (method == QLatin1String("tools/list")) {
        refreshGroups();
        return rpcResult(id, QJsonObject{{"tools", m_tools.mcpToolsJson(exposed())}});
    }
    if (method == QLatin1String("tools/call"))
        return rpcResult(id, callTool(params.value("name").toString(), params.value("arguments").toObject()));
    return rpcError(id, -32601, QStringLiteral("method not found: %1").arg(method));
}

QJsonObject QodeMcp::callTool(const QString &name, const QJsonObject &args)
{
    auto failed = [](const QString &text) {
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}, {"isError", true}};
    };
    if (!isReady())
        return failed(QStringLiteral("no project is open in QODE"));
    if (!exposed().contains(name))
        return failed(QStringLiteral("unknown tool '%1'").arg(name));
    m_tools.resetCancel();
    const agent::ToolResult r = m_tools.execute(name, args);
    QJsonArray content{QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(r.response).toJson(QJsonDocument::Compact))}}};
    for (const QJsonValue &v : r.images) {
        const QJsonObject image = v.toObject();
        content.append(QJsonObject{{"type", "image"}, {"data", image.value("data")}, {"mimeType", image.value("mimeType")}});
    }
    return QJsonObject{{"content", content}, {"isError", !r.ok}};
}

// The server's instructions, which the CLI puts into the model's system prompt.
QString QodeMcp::instructions() const
{
    QString text = QStringLiteral(
        "You run inside QODE, the IDE the user is looking at. These tools see and drive it.\n"
        "- Errors: after editing a file, call get_diagnostics with its path: the language server's errors and warnings of the "
        "current text (QODE opens the file for its server). get_problems lists everything QODE knows is wrong: language "
        "servers' reports and compiler errors from QODE's terminal. Both are cheaper than a build.\n"
        "- terminal_output shows what the user ran in QODE's terminal (\"why did that fail?\").\n"
        "- open_file shows the user a file at a line; show_panel shows parts of the window.\n"
        "- Where a tool's text says run_command, use your Bash tool.");
    if (m_groups.contains(QLatin1String("servers")))
        text += QStringLiteral(
            "\n- QODE runs this project's servers (web dev server, an Expo app's web version, Spring Boot, Ktor). Before starting "
            "a server or asking the user to, call server_status: one that is running is already up at its url. Start, restart "
            "and stop them with server_start / server_restart / server_stop, never with Bash: a second dev server takes another "
            "port and QODE's preview does not show it, and Bash waits for a command that never ends. server_log has their "
            "output (build errors, stack traces).");
    if (m_groups.contains(QLatin1String("preview")))
        text += QStringLiteral(
            "\n- QODE shows the dev server's page in its web preview. preview_status says whether it is shown and what it shows; "
            "show_panel web_preview opens it. After a visual change, look at it with preview_screenshot (preview_set_viewport "
            "for phone / tablet / desktop) before you answer, and read preview_console when the page is blank or broken. "
            "preview_click / preview_type / preview_eval try what you built.");
    return text;
}

} // namespace claudecode
