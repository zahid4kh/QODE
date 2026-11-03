#pragma once

#include "claudecode/ClaudeCodeCli.h"

#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace claudecode {

// A card in the transcript that shows what a control request answered (/usage, /mcp) instead of the CLI's plain text:
// a title row with a summary and a refresh button, and a body rebuilt from the data (and again when the theme changes).
class InfoCard : public QFrame
{
    Q_OBJECT
public:
    explicit InfoCard(const QString &title, QWidget *parent = nullptr);

    void setLoading(const QString &text);
    void setError(const QString &text);
    void setMessage(const QString &text, bool error = false); // a line under the body ("" = none)

signals:
    void refreshRequested();

protected:
    void showData();                   // the data arrived: build() the body
    void rebuild();                    // clears the body and builds it again for the current state
    virtual void build(QVBoxLayout *body) = 0; // widgets go into body->parentWidget()
    void setSummary(const QString &text);

private:
    QVBoxLayout *m_layout;
    QWidget *m_content = nullptr;
    enum class State { Loading, Error, Ready };
    QLabel *m_summary;
    QLabel *m_message;
    QToolButton *m_refresh;
    State m_state = State::Loading;
    QString m_stateText;
};

// /usage: the account (from `claude auth status`), the plan's rate limits with a bar each, usage credits, and what
// used them ("get_usage": recommendations and the skills / subagents / plugins / MCP servers behind them, per day or week).
class UsageCard : public InfoCard
{
    Q_OBJECT
public:
    explicit UsageCard(QWidget *parent = nullptr);
    void setUsage(const AuthStatus &account, const QJsonObject &usage, double conversationCost);

protected:
    void build(QVBoxLayout *body) override;

private:
    AuthStatus m_account;
    QJsonObject m_usage;
    double m_cost = 0;
    bool m_week = false;
};

// /mcp: every MCP server ("mcp_status") with its status, transport and tools (a click on a row lists them), Reconnect for
// a failed one and Enable / Disable. While a server is still connecting the card asks again by itself a few times.
// The QODE chat shows the same card for its own servers, with a link to its MCP dialog.
class McpCard : public InfoCard
{
    Q_OBJECT
public:
    explicit McpCard(QWidget *parent = nullptr);
    void setServers(const QJsonArray &servers);
    void setEmptyText(const QString &text) { m_emptyText = text; }
    void setManageLink(const QString &text) { m_manageText = text; } // "" = none

signals:
    void reconnectRequested(const QString &name);
    void toggleRequested(const QString &name, bool enabled);
    void manageRequested();

protected:
    void build(QVBoxLayout *body) override;

private:
    QJsonArray m_servers;
    QStringList m_open; // servers whose tool list is shown
    int m_polls = 0;
    QString m_emptyText, m_manageText;
};

} // namespace claudecode
