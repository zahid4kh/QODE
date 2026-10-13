#pragma once

#include "ai/AiChat.h"

#include <QHash>
#include <QSet>
#include <QWidget>

class AiAgent;
class AiInput;
class AiMentionPopup;
class EditorManager;
class MarkdownView;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class QTimer;

// Right-hand chat panel: describe a change, Gemini plans it, edits the files of the project and reports what it did.
// Every prompt makes several API calls (see AiAgent). Only usable in a git repository, so a bad edit can always be undone.
class AiPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AiPanel(QWidget *parent = nullptr);

    void setEditors(EditorManager *editors);
    void setProject(const QString &root);          // "" = no project
    void setGitRepo(bool isRepo);
    void setProjectFiles(const QStringList &absolutePaths);
    void focusInput();

signals:
    void closeRequested();
    void openFileRequested(const QString &absolute);
    void gitInitRequested();

private:
    // session
    void startNewChat();
    void loadSession(const QString &id);
    void deleteCurrentChat();
    void saveSession();
    void rebuildHistoryMenu();
    // sending
    void send();
    void stop();
    void setBusy(bool busy);
    QString focusedFile() const;
    QStringList mentionedFiles(const QString &prompt) const;
    // mentions
    void updateMentions(const QString &query);
    void closeMentions();
    // view
    void rebuild();
    QWidget *userBubble(const AiMessage &m);
    QWidget *replyBubble(const AiMessage &m);
    QWidget *liveBubble();
    QWidget *emptyHint();
    QString markdownOf(const AiMessage &m) const;
    QString liveMarkdown() const;
    void followLink(const QUrl &url);
    void scrollToEnd();
    void updateNotice();
    void updateContext();
    void applyTheme();
    QString relative(const QString &absolute) const;

    AiAgent *m_agent;
    AiChatStore m_store;
    AiSession m_session;
    EditorManager *m_editors = nullptr;
    QString m_root;
    bool m_gitOk = false;
    bool m_keyOk = false;
    bool m_busy = false;
    QStringList m_files;          // project-relative
    QSet<QString> m_fileSet;
    QStringList m_livePlan;
    QString m_liveStatus;
    MarkdownView *m_live = nullptr;
    QString m_pendingModel;

    QLabel *m_title;
    QToolButton *m_historyBtn, *m_newBtn, *m_closeBtn;
    QComboBox *m_model;
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
};
