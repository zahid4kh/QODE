#pragma once

#include "agent/AgentSession.h"
#include "agent/AgentTools.h"

#include <QHash>
#include <QSet>
#include <QWidget>

class AiInput;
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
    void focusInput();
    void editApiKey();

signals:
    void closeRequested();
    void openFileRequested(const QString &absolute);
    void gitInitRequested();

private:
    // sessions
    void startNewChat();
    void loadSession(const QString &id);
    void deleteCurrentChat();
    void saveSession();
    void rebuildHistoryMenu();
    // running
    void send();
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
