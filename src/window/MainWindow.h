#pragma once

#include "git/GitTypes.h"
#include "lsp/LspTypes.h"
#include "project/Project.h"

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>

class CodeEditor;
class DevServerBar;
class DiffDialog;
class Document;
class QTimer;
class GitPanel;
class MediaPanel;
class PackagesDialog;
class ReferencesDialog;
class MarkdownPreview;
class GitRepository;
class LspManager;
class EditorManager;
class ProjectManager;
class ProjectExplorer;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class SideSections;
class BranchButton;
class QToolButton;
class TerminalPanel;
class ProjectFiles;
class MoveController;
class SearchPanel;
class TasksPanel;
struct SearchOptions;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openInitialPaths(const QStringList &paths, bool restoreLastSession = true);

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
    QJsonObject currentLayout() const;
    void applyLayout(const QJsonObject &layout);
    void restoreSession();

    void newWindow();
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
    void togglePreview();
    void updatePreview();
    void followPreview();
    void openRightPanel();
    void hideMedia();
    void runCurrentFile();
    void configureRun();
    void updateRunToolbar();
    void updatePythonUi();           // shows the Python Packages button only while the project has a virtual environment
    void showPythonPackages();
    void restartPythonServer();
    QStringList unresolvedPythonImports(Document *doc, int startLine, int startColumn, int endLine, int endColumn) const;
    void askNestedWebProject(const QStringList &dirs);
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

    // Language servers (LSP)
    void createLspMenu(QMenu *menu);
    void rebuildLspMenu();
    void updateLspStatus();
    void showLspInstallHelp(const QString &serverId);
    void installLspServer(const QString &serverId);
    void removeLspServer(const QString &serverId);
    void restartApplication();
    void chooseLspServerPath(const QString &serverId);
    void showLspLog(const QString &serverId);
    void editCompilerFlags();
    void syncBuildFiles();
    void goToDefinition(CodeEditor *editor, int line, int column);
    void showCodeActions(Document *doc, CodeEditor *editor, int startLine, int startColumn, int endLine, int endColumn);
    bool applyWorkspaceEdit(const QJsonObject &edit);
    void removeUnusedImports(CodeEditor *editor);
    void renameSymbol();
    void findReferences();
    void requestColors(Document *doc, CodeEditor *editor);
    void pickColor(Document *doc, CodeEditor *editor, int index);
    Document *documentOf(CodeEditor *editor) const;

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
    MoveController *m_mover = nullptr;
    QStringList m_recentFiles; // most recently active first
    GitRepository *m_git;
    LspManager *m_lsp;
    QMenu *m_lspMenu;
    QTimer *m_importTimer = nullptr; // recomputes the unused imports once diagnostics settle
    QString m_importPath;
    QToolButton *m_lspButton;
    QPointer<ReferencesDialog> m_refsDialog;
    QHash<CodeEditor *, QVector<LspColor>> m_colorData; // the colour literals behind each editor's swatches
    QHash<Document *, qint64> m_colorAsked;            // when colours were last requested for a document (throttle)
    bool m_lspHintShown = false;
    QTimer *m_venvTimer = nullptr;    // notices a virtual environment created or deleted outside QODE
    QString m_activeVenv;             // last venv seen by updatePythonUi
    QPointer<PackagesDialog> m_pkgDialog;
    GitPanel *m_gitPanel;
    SearchPanel *m_searchPanel;
    TasksPanel *m_tasks;
    QHash<QString, QList<int>> m_bookmarks; // path -> 0-based lines (persisted)
    SideSections *m_side;
    ProjectExplorer *m_explorer;
    EditorManager *m_editors;
    TerminalPanel *m_terminal;
    QSplitter *m_hsplit;
    MediaPanel *m_media;
    MarkdownPreview *m_md;
    QStackedWidget *m_rightStack; // media panel | markdown preview
    QPointer<Document> m_previewDoc; // the Markdown / SVG file the right panel is previewing live
    QMetaObject::Connection m_previewConn;
    QTimer *m_previewTimer;
    int m_mediaWidth = 420;
    int m_sideWidth = 250;
    QSplitter *m_vsplit;
    int m_terminalHeight = 240;

    QAction *m_newWindowAct, *m_newProjectAct, *m_openProjectAct, *m_closeProjectAct, *m_exitAct;
    QAction *m_newFileAct, *m_openFileAct, *m_saveAct, *m_saveAsAct, *m_saveAllAct, *m_closeFileAct;
    QAction *m_undoAct, *m_redoAct, *m_cutAct, *m_copyAct, *m_pasteAct, *m_selectAllAct, *m_findAct, *m_replaceAct;
    QAction *m_projNewFileAct, *m_projNewFolderAct, *m_openProjectFolderAct;
    QPointer<QDialog> m_themeEditor;
    void showThemeEditor();
    QAction *m_explorerAct, *m_terminalAct, *m_runAct, *m_runConfigAct, *m_pythonPkgAct, *m_venvAct, *m_fullscreenAct, *m_wordWrapAct;
    QAction *m_nextTabAct, *m_prevTabAct, *m_splitRightAct, *m_splitDownAct, *m_aboutAct, *m_paletteAct, *m_quickOpenAct, *m_gotoLineAct, *m_gotoSymbolAct, *m_gotoDefinitionAct, *m_renameAct, *m_findRefsAct, *m_searchAct, *m_matchBracketAct, *m_indentGuidesAct, *m_stickyAct, *m_blameInlineAct, *m_bookmarkToggleAct, *m_bookmarkNextAct, *m_bookmarkPrevAct, *m_showBookmarksAct, *m_showTodosAct, *m_previewAct, *m_newTerminalAct, *m_blameGutterAct, *m_minimapAct, *m_breadcrumbsAct, *m_hiddenFilesAct;
    QMenu *m_recentMenu;
    DevServerBar *m_serverBar = nullptr;
    QAction *m_serverBarAct = nullptr;
    QToolBar *m_mainToolBar = nullptr;
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
