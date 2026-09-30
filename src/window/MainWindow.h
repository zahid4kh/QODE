#pragma once

#include "git/GitTypes.h"
#include "project/Project.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class DiffDialog;
class Document;
class GitPanel;
class MediaPanel;
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
class ProjectFiles;
class SearchPanel;
class TasksPanel;
struct SearchOptions;

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
    void showTerminal();
    void showMedia(const QString &path);
    void hideMedia();
    void runCurrentFile();
    void configureRun();
    void about();
    void showCommandPalette();
    void showGoToSymbol();
    void showQuickOpen(const QString &initialQuery = {});
    void noteRecentFile();
    void showSearch();
    void showTasks(bool bookmarks);
    void applyBookmarks(const QString &path, const QList<int> &lines);
    void gotoBookmark(bool next);
    QString lineTextOf(const QString &path, int line) const;
    void refreshRecentProjects();
    void openRecentProject(const QString &path);
    void replaceInFiles(const QStringList &paths, const SearchOptions &options, const QString &replacement);
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
    ProjectFiles *m_projectFiles;
    QStringList m_recentFiles; // most recently active first
    GitRepository *m_git;
    GitPanel *m_gitPanel;
    SearchPanel *m_searchPanel;
    TasksPanel *m_tasks;
    QHash<QString, QList<int>> m_bookmarks; // path -> 0-based lines (persisted)
    QWidget *m_side;
    QTabBar *m_sideTabs;
    QStackedWidget *m_sideStack;
    ProjectExplorer *m_explorer;
    EditorManager *m_editors;
    Terminal *m_terminal;
    QSplitter *m_hsplit;
    MediaPanel *m_media;
    int m_mediaWidth = 420;
    QSplitter *m_vsplit;
    int m_terminalHeight = 240;

    QAction *m_newProjectAct, *m_openProjectAct, *m_closeProjectAct, *m_exitAct;
    QAction *m_newFileAct, *m_openFileAct, *m_saveAct, *m_saveAsAct, *m_saveAllAct, *m_closeFileAct;
    QAction *m_undoAct, *m_redoAct, *m_cutAct, *m_copyAct, *m_pasteAct, *m_selectAllAct, *m_findAct, *m_replaceAct;
    QAction *m_projNewFileAct, *m_projNewFolderAct, *m_openProjectFolderAct;
    QAction *m_explorerAct, *m_terminalAct, *m_runAct, *m_runConfigAct, *m_venvAct, *m_fullscreenAct, *m_wordWrapAct, *m_darkThemeAct, *m_lightThemeAct;
    QAction *m_nextTabAct, *m_prevTabAct, *m_aboutAct, *m_paletteAct, *m_quickOpenAct, *m_gotoLineAct, *m_gotoSymbolAct, *m_searchAct, *m_matchBracketAct, *m_indentGuidesAct, *m_stickyAct, *m_blameInlineAct, *m_bookmarkToggleAct, *m_bookmarkNextAct, *m_bookmarkPrevAct, *m_showBookmarksAct, *m_showTodosAct, *m_blameGutterAct, *m_minimapAct, *m_breadcrumbsAct;
    QMenu *m_recentMenu;
    QAction *m_autoSaveOffAct, *m_autoSaveDelayAct, *m_autoSaveFocusAct, *m_trimAct, *m_finalNewlineAct, *m_formatOnSaveAct, *m_formatAct;
    QAction *m_foldAct, *m_unfoldAct, *m_foldAllAct, *m_unfoldAllAct;

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
