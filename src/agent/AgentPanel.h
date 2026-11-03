#pragma once

#include "agent/AgentJournal.h"
#include "agent/AgentSession.h"
#include "agent/AgentTools.h"
#include "ai/AiEnv.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QPointer>
#include <QWidget>

class AiInput;
class AiQuote;
class QHBoxLayout;
class ClaudeClient;
class GeminiClient;
class LspManager;
class QTimer;
class AiMentionPopup;
class EditorManager;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace mcp {
class Manager;
}

namespace agent {

class AgentRun;
class AgentTurnView;

// The AI panel's agent: describe a task and a Gemini or Claude model carries it out on its own with tool calls - it
// searches and reads the project, edits files through the editor tabs, runs commands (after asking) and reports.
// Its chats are kept apart from the plan-and-edit chat (own folder, own format).
class AgentPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AgentPanel(QWidget *parent = nullptr);

    void setEditors(EditorManager *editors);
    void setProject(const QString &root);          // "" = no project
    void setGitRepo(bool isRepo);
    void setProjectFiles(const QStringList &absolutePaths);
    void setLsp(LspManager *lsp) { m_lsp = lsp; }
    // Set by MainWindow: runs the IDE tools (panels, theme, editor options, language servers; see AgentTools::Hooks::ide).
    void setIdeHandler(std::function<QJsonObject(const QString &, const QJsonObject &)> handler) { m_ide = std::move(handler); }
    void focusInput();
    void editApiKey(); // the key of the current model's provider
    void editApiKeyFor(AiEnv::Provider provider);
    // The MCP Servers dialog (View menu, the header's MCP button, the /mcp card); `select` = a server to show.
    void showMcpServers(const QString &select = QString());
    // "Fix with QODE": a new chat (this one when it is empty) about an error. `quote` is the error as the user sees it in
    // their bubble (`quoteLead` tells the model where it is from), `files` the project-relative files it names (pointed
    // at), `request` the text the model gets after the quote (the lines around the error included). While a run is going
    // it waits in the input instead.
    void startFix(const QString &display, const QString &request, const QStringList &files, const QString &quote, const QString &quoteLead);

signals:
    void openFileRequested(const QString &absolute, int line = 0);
    void gitInitRequested();
    void projectChanged(); // the agent (or an undo) changed files: refresh git status and the file index

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // sessions
    void startNewChat();
    void loadSession(const QString &id);
    void deleteCurrentChat();
    void saveSession();
    void rebuildHistoryMenu();
    void loadJournal();
    void saveJournal();
    // running
    void send();
    // `quoteLead`: where the quote is from, for the model ("" = its earlier reply).
    void startRun(const QString &display, const QString &request, const QStringList &mentioned, const QString &quote = QString(),
                  const QString &quoteLead = QString());
    void askAbout(const QString &text); // "Ask about this": sent right away, the draft stays
    void setQuote(const QString &text); // "Quote this": goes with the next message ("" = none)
    bool runSlash(const QString &prompt);
    void addNote(const QString &text);
    void compact();
    void undoFromUi(int turn, const QString &what);
    void redoFromUi();
    QString collectDiagnostics(const QString &abs, QVector<Diag> *rows, bool afterEdit, bool settle);
    bool writeText(const QString &abs, const QString &text);
    void updateSlash();
    void scheduleProjectChanged();
    QWidget *turnFooter(const Message &m);
    QWidget *summaryCard(const Message &m);
    QWidget *mcpCard();                // /mcp: the servers, live
    void rebuildMcpMenu();
    void updateMcpButton();
    void prepareMcpTools();
    void prepareToolGroups();            // the connected servers' tools for the next run
    void updateUsage();                // the header's meter: tokens, cache hits, cost (AgentPanelUsage.cpp)
    void rebuildUsageMenu();
    void stop();
    void abandonRun();
    void setBusy(bool busy);
    void finishRun(const QString &error);
    bool approve(const QString &tool, const QString &title, const QString &detail);
    QString focusedFile() const;
    QStringList mentionedFiles(const QString &prompt) const;
    QString contextFor(const QStringList &mentioned) const;
    // mentions
    void updateMentions(const QString &query);
    void closeMentions();
    // view
    void rebuild();
    QWidget *userBubble(const Message &m);
    void wireTurn(AgentTurnView *view);
    QWidget *emptyHint();
    void followLink(const QUrl &url);
    void updateNotice();
    void recallPrompt(int delta); // Up / Down in the input: the chat's earlier messages
    void updateContext();
    AiEnv::Provider provider() const { return AiEnv::providerForModel(m_modelName); }
    void setModel(const QString &name);
    // setModel for a model the user picked: a switch to the other provider needs a new chat (asked first).
    void chooseModel(const QString &name);
    // A chat stays with the provider it was held with: after loading one, the model follows it.
    void followChatProvider();
    void updateModelButton();
    void rebuildModelMenu();
    void rebuildIndexMenu();
    void updateIndexButton();
    void applyTheme();
    QString relative(const QString &absolute) const;

    AgentTools m_tools;
    CodeIndex *m_index;            // the codebase index behind semantic_search
    mcp::Manager *m_mcp;           // MCP servers: more tools for the chat
    Journal m_journal;
    LspManager *m_lsp = nullptr;
    std::function<QJsonObject(const QString &, const QJsonObject &)> m_ide;
    GeminiClient *m_gemini;
    ClaudeClient *m_claude;
    int m_compactGen = 0;
    bool m_compacting = false;
    bool m_slash = false;
    QTimer *m_changedTimer;
    QTimer *m_clockTimer;         // ticks the elapsed time in the status line while busy
    AgentRun *m_run = nullptr;
    Store m_store;
    Session m_session;
    EditorManager *m_editors = nullptr;
    QString m_root;
    bool m_gitOk = false;
    bool m_keyOk = false;
    bool m_busy = false;
    QStringList m_files;          // project-relative, for @-mentions
    QSet<QString> m_fileSet;
    QStringList m_allFiles;       // absolute, for the tools
    QSet<QString> m_alwaysAllow;  // tools the user allowed for the rest of this chat
    QString m_status;
    int m_recallPos = 0;

    Message m_live;               // the assistant turn being written
    RequestStats m_liveRequest;   // its request being streamed (not in m_live.requestLog yet)
    AgentTurnView *m_liveView = nullptr;
    QPointer<AgentTurnView> m_lastTurn; // its Copy button is always shown
    QElapsedTimer m_runClock;     // since the live turn started

    QToolButton *m_historyBtn, *m_keyBtn, *m_newBtn, *m_indexBtn, *m_mcpBtn, *m_usageBtn;
    QPushButton *m_modelBtn;
    QMenu *m_modelMenu;
    QLabel *m_modelIcon, *m_modelArrow, *m_modelText;
    QLineEdit *m_customModel = nullptr;
    QString m_modelName;
    QFrame *m_notice = nullptr;
    QLabel *m_noticeText;
    QPushButton *m_noticeBtn;
    QScrollArea *m_scroll;
    QWidget *m_list;
    QVBoxLayout *m_listLayout;
    QHBoxLayout *m_modelRow = nullptr;
    QHBoxLayout *m_noticeRow = nullptr;
    QVBoxLayout *m_bottom = nullptr;
    QLabel *m_context;
    AiInput *m_input;
    AiQuote *m_quote;
    QString m_quoteLead; // where the quote waiting in the input is from ("" = an earlier reply)
    QToolButton *m_send;
    QFrame *m_inputBox;
    AiMentionPopup *m_popup;
    bool m_stick = true;          // the transcript follows new output until the user scrolls up
};

} // namespace agent
