#pragma once

#include "agent/AgentJournal.h"
#include "agent/AgentSession.h"
#include "agent/AgentTools.h"

#include <QHash>
#include <QSet>
#include <QWidget>

class AiInput;
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

namespace agent {

class AgentRun;
class AgentTurnView;

// The "Agent" tab of the AI panel: describe a task and Gemini carries it out on its own with function calling - it
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
    void focusInput();
    void editApiKey();

signals:
    void closeRequested();
    void openFileRequested(const QString &absolute);
    void gitInitRequested();
    void projectChanged(); // the agent (or an undo) changed files: refresh git status and the file index

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
    void startRun(const QString &display, const QString &request, const QStringList &mentioned);
    bool runSlash(const QString &prompt);
    void addNote(const QString &text);
    void compact();
    void undoFromUi(int turn, const QString &what);
    void redoFromUi();
    QString collectDiagnostics(const QString &abs, QVector<Diag> *rows);
    bool writeText(const QString &abs, const QString &text);
    void updateSlash();
    void scheduleProjectChanged();
    QWidget *turnFooter(const Message &m);
    QWidget *summaryCard(const Message &m);
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
    QWidget *emptyHint();
    void followLink(const QUrl &url);
    void updateNotice();
    void updateContext();
    void setModel(const QString &name);
    void rebuildModelMenu();
    void applyTheme();
    QString relative(const QString &absolute) const;

    AgentTools m_tools;
    Journal m_journal;
    LspManager *m_lsp = nullptr;
    GeminiClient *m_gemini;
    int m_compactGen = 0;
    bool m_compacting = false;
    bool m_slash = false;
    QTimer *m_changedTimer;
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

    Message m_live;               // the assistant turn being written
    AgentTurnView *m_liveView = nullptr;

    QToolButton *m_historyBtn, *m_keyBtn, *m_newBtn, *m_closeBtn;
    QPushButton *m_modelBtn;
    QMenu *m_modelMenu;
    QLabel *m_modelArrow, *m_modelText;
    QLineEdit *m_customModel = nullptr;
    QString m_modelName;
    QFrame *m_notice;
    QLabel *m_noticeText;
    QPushButton *m_noticeBtn;
    QScrollArea *m_scroll;
    QWidget *m_list;
    QVBoxLayout *m_listLayout;
    QLabel *m_context;
    AiInput *m_input;
    QToolButton *m_send;
    QFrame *m_inputBox;
    AiMentionPopup *m_popup;
    bool m_stick = true;          // the transcript follows new output until the user scrolls up
};

} // namespace agent
