#pragma once

#include "git/GitTypes.h"
#include "project/Project.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class DiffDialog;
class Document;
class GitPanel;
class GitRepository;
class EditorManager;
class ProjectManager;
class ProjectExplorer;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabBar;
class BranchButton;
class QToolButton;
class Terminal;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openInitialPaths(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();
    void createMenus();
    void createToolBar();
    void createStatusBar();
    void restoreSettings();
    void saveSession();
    void restoreSession();

    void newProject();
    void openProject();
    void openProjectPath(const QString &path);
    bool closeProject();
    void onProjectOpened(const Project &p);
    void onProjectClosed();

    void newFile();
    void openFile();
    void updateTitle();
    void updateStatus();
    void updateActions();
    void toggleTerminal();
    void about();
    void showCommandPalette();
    void forwardToFocus(const char *slot);

    // Git
    void createGitActions();
    void createGitMenu(QMenu *menu);
    void setupGit();
    void onGitStatusChanged();
    void refreshGutter(Document *doc);
    void refreshAllGutters();
    void showDiff(const QString &path, GitDiffMode mode);
    void showSourceControl();
    void showChangesForCurrentFile();
    void discardPaths(const QStringList &paths);
    QString currentFilePath() const;

    ProjectManager *m_projects;
    GitRepository *m_git;
    GitPanel *m_gitPanel;
    QWidget *m_side;
    QTabBar *m_sideTabs;
    QStackedWidget *m_sideStack;
    ProjectExplorer *m_explorer;
    EditorManager *m_editors;
    Terminal *m_terminal;
    QSplitter *m_hsplit;
    QSplitter *m_vsplit;
    int m_terminalHeight = 240;

    QAction *m_newProjectAct, *m_openProjectAct, *m_closeProjectAct, *m_exitAct;
    QAction *m_newFileAct, *m_openFileAct, *m_saveAct, *m_saveAsAct, *m_saveAllAct, *m_closeFileAct;
    QAction *m_undoAct, *m_redoAct, *m_cutAct, *m_copyAct, *m_pasteAct, *m_selectAllAct, *m_findAct, *m_replaceAct;
    QAction *m_projNewFileAct, *m_projNewFolderAct, *m_openProjectFolderAct;
    QAction *m_explorerAct, *m_terminalAct, *m_fullscreenAct, *m_wordWrapAct, *m_darkThemeAct, *m_lightThemeAct;
    QAction *m_nextTabAct, *m_prevTabAct, *m_aboutAct, *m_paletteAct;

    QAction *m_scmAct, *m_gitRefreshAct, *m_gitFetchAct, *m_gitPullAct, *m_gitPushAct, *m_gitInitAct, *m_gitNewBranchAct;
    QAction *m_gitStageFileAct, *m_gitUnstageFileAct, *m_gitDiscardFileAct, *m_gitDiffFileAct, *m_nextChangeAct, *m_prevChangeAct;
    QWidget *m_gitStatusWidget;
    BranchButton *m_branchButton;
    QToolButton *m_aheadBtn, *m_behindBtn;
    QLabel *m_changesPill;
    QString m_lastHead, m_lastRoot;
    QHash<QString, QPointer<DiffDialog>> m_diffs;

    QLabel *m_fileLabel, *m_langLabel, *m_encLabel, *m_eolLabel, *m_posLabel, *m_modeLabel;
};
