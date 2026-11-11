#pragma once

#include "agent/AgentSession.h"
#include "claudecode/ClaudeCodeChanges.h"
#include "claudecode/ClaudeCodePrompt.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>

class AiInput;
class AiQuote;
class EditorManager;
class AiMentionPopup;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace agent {
class AgentTurnView;
struct ToolResult;
}

namespace claudecode {

class Auth;
class ContextMeter;
class Session;
class TaskList;

// The Claude Code view of the chat area: a graphical front end for the `claude` CLI. It checks that Claude Code is
// installed and signed in (and walks through the sign-in), then runs one conversation at a time in the project folder
// (ClaudeCodeSession) and shows it with the same turn view as the QODE chat: thinking, text, tool calls with diff cards
// for edits, and permission prompts as cards with Allow / Deny. See dev-docs/claude-code.md.
class ClaudeCodePanel : public QWidget
{
    Q_OBJECT
public:
    explicit ClaudeCodePanel(QWidget *parent = nullptr);
    ~ClaudeCodePanel() override;

    void setProject(const QString &root); // "" = no project
    void setProjectFiles(const QStringList &absolutePaths);
    void setEditors(EditorManager *editors); // its selection goes along with the next message
    void setGitDirty(bool dirty);            // the project has uncommitted changes: offered as a suggestion
    void focusInput();

signals:
    void showRequested(); // a desktop notification was clicked: raise the window and show this page
    void openFileRequested(const QString &absolute, int line = 0);
    void projectChanged(); // Claude Code edited files or ran a command: refresh git status and the file index

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Setup { Checking, NotInstalled, SignIn, SigningIn };

    // pages
    void buildSetupPage();
    void buildChatPage();
    void updatePage();
    void showSetup(Setup state, const QString &message = QString());
    bool ready() const;
    void rebuildAccountMenu();
    void startLogin(bool console);
    // conversation
    void newConversation();
    void rebuildHistoryMenu();
    void fitColumn(); // the centred column on a wide window
    void openConversation(const QString &id);
    void send();                       // the input as it is: sent now, or queued while a turn runs
    void dispatch(const Prompt &prompt);
    void askAbout(const QString &text); // "Ask about this": sent (or queued) right away, the draft stays
    void setQuote(const QString &text); // "Quote this": goes with the next message ("" = none)
    void sendNext();                   // the first queued message
    void afterTurn(bool ok);           // a turn ended: the queue goes on when it ended well (or "Send now" asked)
    // desktop notifications, while QODE is not the active window or this page is hidden
    bool notifyWanted() const;
    void notifyTurnEnd(bool ok);
    void notifyWaiting(const QJsonObject &request);
    void closeNotifications(bool all = true); // all = false: only "waiting"
    void updateQueue();
    Selection editorSelection() const;
    QString editorFile() const; // the active tab's file, when it may go to Claude Code
    void updateSelection();
    void updateSendButton();
    bool hasDraft() const;
    bool runLocalSlash(const QString &text);
    // /usage and /mcp: a card filled from a control request instead of the CLI's text; they run at once, even mid-turn
    bool showInfo(const QString &text);
    void loadUsage(class UsageCard *card);
    void loadMcp(class McpCard *card);
    void query(const QJsonObject &request, std::function<void(const QJsonObject &, const QString &)> done);
    void ensureSession();
    void stopTurn();
    void finishTurn();
    void setBusy(bool busy);
    void onMessage(const QJsonObject &o);
    void onStreamEvent(const QJsonObject &event);
    void onResult(const QJsonObject &o);
    void onExited(int code, const QString &error);
    // content
    void addAssistantContent(agent::AgentTurnView *view, const QJsonArray &content, bool textStreamed, bool subagent);
    void addToolResults(agent::AgentTurnView *view, const QJsonArray &content, const QJsonObject &toolUseResult);
    void noteChange(const QString &name, const QJsonObject &input, const QJsonObject &structured, const agent::ToolResult &result);
    void flushChanges(); // the "files changed" card of the turn that just ended
    QString describeTool(const QString &name, const QJsonObject &input) const;
    agent::ToolResult toolResult(const QString &name, const QJsonObject &input, const QJsonObject &block, const QJsonObject &structured) const;
    // permissions
    void onPermission(const QString &id, const QJsonObject &request);
    QWidget *permissionCard(const QString &id, const QJsonObject &request);
    QWidget *questionCard(const QString &id, const QJsonObject &request);
    QWidget *planCard(const QString &id, const QJsonObject &request);
    void answer(const QString &id, const QJsonObject &decision);
    void cancelPermissions(const QString &why);
    // view
    agent::AgentTurnView *newTurnView();
    // `context`: one line per attachment ("+ selection a.cpp:3–9").
    QWidget *userBubble(const QString &text, const QVector<ImageAttachment> &images = {}, const QStringList &context = {},
                        const QString &quote = QString());
    void addWidget(QWidget *w);
    void addNote(const QString &text, bool error = false);
    void clearTranscript();
    void showEmptyHint();
    void refreshEmptyHint(); // its suggestions follow the project, the sign-in and the git status
    QWidget *suggestionList(QWidget *parent);
    void updateSlash();
    void recallPrompt(int delta); // Up / Down in the input
    void notePrompt(const QString &text);
    void updateMentions(const QString &query);
    void closeMentions();
    void updatePickers();
    void updateStats();
    int contextWindow() const; // of the conversation's model: reported by the CLI, else 1M for "[1m]" ids, else 200k
    void updateMeter();
    void compactConversation(); // the meter was clicked: /compact, queued while a turn runs
    void scheduleProjectChanged();
    void followLink(const QUrl &url);
    QString relative(const QString &path) const;
    void applyTheme();

    Auth *m_auth;
    Session *m_session;
    QString m_root;
    QStringList m_files;            // project-relative, for @-mentions
    bool m_busy = false;
    bool m_checkedOnce = false;
    bool m_gitDirty = false;
    QString m_sessionId;            // the conversation to resume when the process is (re)started
    QString m_model;                // what the session reported (full id), for the turn heading
    QJsonObject m_init;             // the initialize response (commands, …)
    QStringList m_commands;         // "/name   description"
    double m_cost = 0;              // this conversation, as reported by the CLI
    double m_costBefore = 0;        // what earlier processes of this conversation cost
    QString m_limitText;            // usage limit warning
    int m_contextUsed = -1;         // after the last main-thread reply: its prompt plus output; -1 = not known
    int m_contextWindow = 0;        // from the last result's modelUsage; 0 = not reported for this model yet
    bool m_compacted = false;       // the size is unknown because the conversation was just compacted

    // the live turn
    agent::AgentTurnView *m_live = nullptr;
    QPointer<agent::AgentTurnView> m_lastTurn; // its Copy button is always shown
    QHash<QString, int> m_toolRows;           // tool_use id -> row in its view
    QHash<QString, QJsonObject> m_toolInputs; // tool_use id -> {name, input}
    QHash<QString, agent::AgentTurnView *> m_toolViews;
    QString m_curMsgId;                       // the message being streamed
    bool m_gotText = false;                   // the live turn showed some text (else the result text is shown)
    QString m_turnReply, m_turnError;         // how the last turn ended, for its notification
    bool m_stopping = false;
    bool m_codeSubmitted = false;             // sign-in: a code went to the CLI
    QSet<QString> m_streamed;                 // message ids whose text / thinking already arrived as stream events
    QVector<agent::RequestStats> m_done;
    agent::RequestStats m_cur;
    bool m_requesting = false;
    bool m_thinking = false;
    qint64 m_thinkStart = 0;
    int m_streamChars = 0, m_thinkChars = 0;
    QElapsedTimer m_reqClock, m_turnClock;
    QTimer *m_statsTimer;
    QTimer *m_stopTimer;
    QTimer *m_changedTimer;
    QHash<QString, QWidget *> m_permissions; // open permission cards by request id
    QHash<QString, std::function<void(const QJsonObject &, const QString &)>> m_queries; // control requests waiting for an answer
    QVector<FileChange> m_turnChanges;        // files the turn edited, for its "files changed" card
    QVector<Prompt> m_queue;                  // written while a turn ran
    bool m_queueHeld = false;                 // the last turn was stopped or failed: the queue waits for "Send now"
    bool m_sendNow = false;                   // a turn is being stopped to send the first queued message
    EditorManager *m_editors = nullptr;
    QString m_selectionSkip;                  // the selection the user left out, or that was sent already
    QStringList m_recall;                     // prompts for Up / Down: typed in the terminal, the open conversation's, sent here
    int m_recallPos = 0;
    QTimer *m_selectionTimer;

    // widgets
    QToolButton *m_accountBtn, *m_historyBtn, *m_newBtn;
    QStackedWidget *m_pages;
    // setup page
    QWidget *m_setup;
    QVBoxLayout *m_setupCol;
    QLabel *m_setupTitle, *m_setupText, *m_setupError;
    QWidget *m_installRow;
    QWidget *m_signInBox;
    QPushButton *m_useKeyBtn;
    QWidget *m_loginBox;
    QLabel *m_loginText;
    QLineEdit *m_codeEdit;
    QPushButton *m_codeBtn;
    QPushButton *m_retryBtn;
    // chat page
    QScrollArea *m_scroll;
    QWidget *m_list;
    QVBoxLayout *m_listLayout;
    QWidget *m_hint = nullptr;
    QVBoxLayout *m_bottom;
    QLabel *m_statusLabel;
    QFrame *m_inputBox;
    AttachmentBar *m_attach;
    AiQuote *m_quote;
    QueueList *m_queueList;
    TaskList *m_tasks;
    AiInput *m_input;
    QToolButton *m_send;
    QToolButton *m_modelBtn, *m_modeBtn;
    ContextMeter *m_meter;
    QLabel *m_usage;
    AiMentionPopup *m_popup;
    bool m_slash = false;
    bool m_stick = true;
};

} // namespace claudecode
