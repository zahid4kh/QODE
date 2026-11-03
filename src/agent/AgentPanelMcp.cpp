// The QODE chat's side of MCP servers: their tools for each run, the header's MCP button, the /mcp card and the dialog.
#include "AgentPanel.h"

#include "claudecode/ClaudeCodeInfo.h"
#include "mcp/McpDialog.h"
#include "mcp/McpManager.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QJsonArray>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QToolButton>

namespace agent {

namespace {
QIcon statusIcon(const QString &status)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    QColor c = t.textMuted;
    if (status == QLatin1String("connected"))
        c = t.success;
    else if (status == QLatin1String("pending"))
        c = t.warning;
    else if (status == QLatin1String("failed"))
        c = t.danger;
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(4, 4, 8, 8));
    return QIcon(pm);
}

QString statusText(const QJsonObject &s)
{
    const QString status = s.value(QLatin1String("status")).toString();
    const int tools = int(s.value(QLatin1String("tools")).toArray().size());
    if (status == QLatin1String("connected"))
        return tools == 1 ? AgentPanel::tr("1 tool") : AgentPanel::tr("%1 tools").arg(tools);
    if (status == QLatin1String("pending"))
        return AgentPanel::tr("connecting…");
    if (status == QLatin1String("failed"))
        return AgentPanel::tr("failed");
    return AgentPanel::tr("disabled");
}
} // namespace

// Before each run: the connected servers' tools, declared after QODE's own. A tool asks first unless the server marks it
// read-only or the user trusts the server (the approval dialog's "Allow for this chat" then covers that one tool).
void AgentPanel::prepareMcpTools()
{
    QVector<ExtraTool> extra;
    for (const mcp::Tool &t : m_mcp->tools()) {
        ExtraTool e;
        e.name = t.qualified;
        e.description = t.description;
        e.schema = t.schema;
        e.ask = !t.readOnly && !m_mcp->trusted(t.server);
        e.label = QStringLiteral("%1 › %2").arg(t.server, t.name);
        e.run = [this, t](const QJsonObject &args) {
            const mcp::CallResult r = m_mcp->call(t, args, [this] { return m_tools.cancelled(); });
            ToolResult out;
            out.ok = r.ok;
            out.response = r.response;
            out.summary = r.summary;
            out.output = r.output;
            out.images = r.images;
            return out;
        };
        extra << e;
    }
    m_tools.setExtraTools(extra);
}

void AgentPanel::updateMcpButton()
{
    QMap<QString, int> counts;
    const QJsonArray servers = m_mcp->status();
    for (const QJsonValue &v : servers)
        ++counts[v.toObject().value(QLatin1String("status")).toString()];
    QStringList parts;
    if (counts.value(QStringLiteral("connected")))
        parts << tr("%1 connected").arg(counts.value(QStringLiteral("connected")));
    if (counts.value(QStringLiteral("pending")))
        parts << tr("%1 connecting").arg(counts.value(QStringLiteral("pending")));
    if (counts.value(QStringLiteral("failed")))
        parts << tr("%1 failed").arg(counts.value(QStringLiteral("failed")));
    m_mcpBtn->setToolTip(servers.isEmpty() ? tr("MCP servers: tools from other programs (none yet)")
                                           : tr("MCP servers: %1").arg(parts.isEmpty() ? tr("all disabled") : parts.join(QStringLiteral(", "))));
}

void AgentPanel::rebuildMcpMenu()
{
    QMenu *menu = m_mcpBtn->menu();
    menu->clear();
    menu->addSection(tr("MCP servers"));
    if (!m_mcp->hasProject()) {
        menu->addAction(tr("Open a project to use MCP servers"))->setEnabled(false);
    } else {
        const QJsonArray servers = m_mcp->status();
        if (servers.isEmpty())
            menu->addAction(tr("None yet: add one to give QODE more tools"))->setEnabled(false);
        for (const QJsonValue &v : servers) {
            const QJsonObject s = v.toObject();
            const QString name = s.value(QLatin1String("name")).toString();
            QAction *a = menu->addAction(statusIcon(s.value(QLatin1String("status")).toString()),
                                         QStringLiteral("%1  ·  %2").arg(name, statusText(s)));
            a->setToolTip(s.value(QLatin1String("error")).toString().left(300));
            connect(a, &QAction::triggered, this, [this, name] { showMcpServers(name); });
        }
        menu->addSeparator();
        QAction *card = menu->addAction(tr("Show in the Chat (/mcp)"));
        card->setEnabled(!m_busy);
        connect(card, &QAction::triggered, this, [this] { runSlash(QStringLiteral("/mcp")); });
    }
    menu->addAction(tr("Manage MCP Servers…"), this, [this] { showMcpServers(); });
}

QWidget *AgentPanel::mcpCard()
{
    auto *card = new claudecode::McpCard(m_list);
    int importable = 0;
    for (const mcp::Found &f : mcp::claudeCodeServers(m_root))
        importable += f.type == QLatin1String("stdio");
    card->setEmptyText(importable ? tr("No MCP servers yet. Claude Code has %n you can import in one click.", nullptr, importable)
                                  : tr("No MCP servers yet. They give QODE tools from other programs, such as Blender or a database."));
    card->setManageLink(m_mcp->configs().isEmpty() ? tr("Add an MCP server…") : tr("Manage MCP servers…"));
    auto refresh = [this, card] { card->setServers(m_mcp->status()); };
    refresh();
    connect(m_mcp, &mcp::Manager::changed, card, refresh);
    connect(card, &claudecode::McpCard::refreshRequested, card, refresh);
    connect(card, &claudecode::McpCard::reconnectRequested, this, [this](const QString &name) { m_mcp->reconnect(name); });
    connect(card, &claudecode::McpCard::toggleRequested, this, [this](const QString &name, bool on) { m_mcp->setEnabled(name, on); });
    connect(card, &claudecode::McpCard::manageRequested, this, [this] { showMcpServers(); });
    return card;
}

void AgentPanel::showMcpServers(const QString &select)
{
    QWidget *top = window();
    auto *dialog = top->findChild<mcp::McpDialog *>();
    if (!dialog) {
        dialog = new mcp::McpDialog(m_mcp, top);
        dialog->setWindowModality(Qt::WindowModal);
    }
    if (!select.isEmpty())
        dialog->selectServer(select);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace agent
