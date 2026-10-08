#pragma once

#include "GitTypes.h"

#include <QSet>
#include <QWidget>

class BranchButton;
class GitRepository;
class QCheckBox;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// The "Version Control" view: branch/sync toolbar, commit box, staged / unstaged / conflict lists
// with hover actions, and a commit history tab.
class GitPanel : public QWidget
{
    Q_OBJECT
public:
    explicit GitPanel(GitRepository *repo, QWidget *parent = nullptr);

    void focusCommitMessage();
    void showChanges();

    // Fills `menu` with branch switching / creation / deletion entries (also used by the status bar).
    static void populateBranchMenu(GitRepository *repo, QMenu *menu, QWidget *dialogParent);
    static void promptNewBranch(GitRepository *repo, QWidget *dialogParent);
    static bool confirmDiscard(const QList<GitFileChange> &changes, QWidget *parent);

signals:
    void openFileRequested(const QString &path);
    void diffRequested(const QString &path, GitDiffMode mode);

private:
    enum Section { SecConflicts = 0, SecStaged = 1, SecChanges = 2 };
    enum Action { ActStage, ActUnstage, ActDiscard, ActOpen };

    void onRepositoryChanged();
    void onStatusChanged();
    void updateToolbar();
    void applyTheme();
    void rebuildTree();
    void loadHistory();
    void commit();
    void doCommit(const QString &message, bool amend);
    void onAction(const QString &path, int section, int action, bool isSection);
    void openItem(QTreeWidgetItem *item);
    void showTreeMenu(const QPoint &pos);
    void showMoreMenu();
    void discardPaths(const QStringList &paths);
    QList<GitFileChange> changesForSection(int section) const;
    QStringList selectedPaths(int *section = nullptr) const;

    GitRepository *m_repo;
    QStackedWidget *m_pages;   // 0 message / init, 1 repository
    QLabel *m_messageLabel;
    QPushButton *m_initButton;

    BranchButton *m_branchBtn;
    QToolButton *m_pullBtn, *m_pushBtn, *m_refreshBtn, *m_moreBtn;
    QLabel *m_busyLabel;
    QTabBar *m_tabs;
    QStackedWidget *m_body;    // 0 changes, 1 history
    QPlainTextEdit *m_message;
    QPushButton *m_commitBtn;
    QCheckBox *m_amend;
    QTreeWidget *m_tree;
    QLabel *m_noChanges;
    QTreeWidget *m_history;
    QString m_historyHead;
    QString m_prefilledAmend;
    QSet<int> m_collapsed;
};
