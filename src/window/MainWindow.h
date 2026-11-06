#pragma once

#include "git/GitTypes.h"
#include "lsp/LspManager.h"
#include "lsp/LspTypes.h"
#include "project/Project.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QSet>

#include <functional>

#include "lsp/JarSource.h"
#include <QPointer>

class CodeEditor;
class DevServer;
class DevServerBar;
class ExpoBar;
class ExpoConfigDialog;
class ExpoSchema;
class DiffDialog;
class Document;
class QTimer;
class GitPanel;
class MediaPanel;
class PackagesDialog;
class ReferencesDialog;
class CallHierarchyDialog;
class MarkdownPreview;
class GradlePanel;
class MavenCentral;
class MavenPanel;
class GamePanel;
class SpringBar;
class SpringPanel;
class KtorBar;
class KtorPanel;
class HttpPanel;
struct HttpResult;
struct BuildProblem;
struct FailedCommand;
class WebPreview;
class AiHub;
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
class QToolBar;
class QToolButton;
class QProgressBar;
class TerminalPanel;
class ProjectFiles;
class MoveController;
class SearchPanel;
class TasksPanel;
class ScriptCheck;
class DebugSession;
class DebugPanel;
struct DebugSourceBreakpoints;
struct SearchOptions;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

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
    void showEncodingMenu();   // the status bar's encoding: Reopen / Save with Encoding
    void showLineEndingMenu(); // the status bar's LF / CRLF
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
    void wirePreviewScroll();
    void followPreview();
    void openRightPanel();
    void hideMedia();
    // The right-hand island's panels are created when first used (see dev-docs/app.md, "Startup"); these create them.
    MediaPanel *mediaPanel();
    MarkdownPreview *markdownPreview();
    SpringPanel *springPanel();
    WebPreview *webPreview();
    bool rightPanelShows(const QWidget *panel) const; // the island is open on `panel` (false for one not created yet)
    void showInRightPanel(QWidget *panel);            // ends a Markdown / SVG preview and opens the island on `panel`
    void clearPreviews();                             // stops a video, drops the rendered Markdown
    void runCurrentFile();
    void runFile(); // the generic part of runCurrentFile (web server, project or file command)
    void configureRun();
    void updateRunToolbar();
    void onExpoChanged();            // Expo apps appeared / vanished: toolbar, web-server bar and JSON schemas follow
    void pushExpoSchemas();
    void runExpoCommand(const QString &command, const QString &id);
    void showExpoConfig();
    void showExpoSchemaCache();
    void updatePythonUi();
    void updateGradleUi();
    void toggleAiPanel();
    void toggleClaudeCode();
    void showMcpServers();
    void setAgentVisible(bool show);
    void setEditorColumnVisible(bool show);
    void toggleSpringPanel();
    // .http / .rest files (MainWindowHttp.cpp)
    void setupHttp();
    HttpPanel *httpPanel();
    void toggleHttpPanel();
    void showHttpPanel();
    void wireHttp(Document *doc, CodeEditor *ed);
    void updateHttpMarkers(Document *doc);
    void sendHttp(Document *doc, int line);
    void followHttp();
    void newHttpFile();
    void openTextInEditor(const QString &text);
    QString springBaseUrl() const;
    void toggleWebPreview();
    void updatePreviewTargets();     // the web app and the Expo apps the preview can show
    void showPreviewTarget(const QString &id, bool start); // brings up the preview on this target (starting it if asked)
    void startPreviewTarget(const QString &id);
    void addSpringStarters();
    void newSpringComponent(int kind, const QString &dir);
    void newSpringCrud(const QString &dir);
    void updateSpringUi();
    bool springOwns(const QString &file) const; // F5 / Run for this file means the Spring Boot application
    // Ktor servers (MainWindowKtor.cpp)
    void setupKtorPanel();
    KtorPanel *ktorPanel();
    void setupKtorBar(QToolBar *tb);
    bool ktorAvailable() const; // a Ktor server, and no Spring Boot application, in the project
    void updateKtorUi();
    void toggleKtorPanel();
    void addKtorPlugins();
    bool ktorOwns(const QString &file) const; // F5 / Run for this file means the Ktor server
    void toggleGradlePanel();           // shows the Python Packages button only while the project has a virtual environment
    void showPythonPackages();
    void restartPythonServer();
    QStringList unresolvedPythonImports(Document *doc, int startLine, int startColumn, int endLine, int endColumn) const;
    void askNestedWebProject(const QStringList &dirs);
    void about();
    void showStartupTime(); // Help > Startup Time: where the start went (StartupProfile)
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
    bool installLspServer(const QString &serverId); // false when the user closed the installer
    void removeLspServer(const QString &serverId);
    void restartApplication();
    void chooseLspServerPath(const QString &serverId);
    void showLspLog(const QString &serverId);
    void editCompilerFlags();
    void editIncludePaths();
    void syncBuildFiles();
    void goToDefinition(CodeEditor *editor, int line, int column, LspManager::Navigate what = LspManager::Navigate::Definition);
    void showWorkspaceSymbols();
    void showCallHierarchy();
    void showTypeHierarchy();
    void fetchLibrarySources(const JarSource::Coordinates &coords, CodeEditor *editor, int line, int column);
    void showCodeActions(Document *doc, CodeEditor *editor, int startLine, int startColumn, int endLine, int endColumn);
    bool applyWorkspaceEdit(const QJsonObject &edit);
    void removeUnusedImports(CodeEditor *editor);
    void renameSymbol();
    void findReferences();
    void showReferencesAt(CodeEditor *editor, int line, int column);
    // The list of places of Find References (also for "N references" / "N implementations" lenses); `symbol` titles it.
    void showLocations(const QString &symbol, const QVector<LspLocation> &locations, bool implementations = false);

    // "Fix with QODE" from a failed terminal command or Tasks > Problems (MainWindowFix.cpp)
    void createFixActions();
    void fixFailedCommand(const FailedCommand &failure);
    void fixProblems(const QList<BuildProblem> &problems);
    QList<QPair<QString, int>> fixLocations(const QString &text, const QString &folder) const;
    QString fixExcerpt(const QString &path, const QList<int> &lines) const;
    void startFixChat(const QString &display, const QString &intro, const QString &quote, const QString &quoteLead,
                      const QList<QPair<QString, int>> &spots);

    // C / C++: Switch Header/Source, Generate Definition, #include folders, terminal file links (MainWindowCpp.cpp)
    void createCppActions();
    void wireCpp(Document *doc, CodeEditor *editor);
    void switchHeaderSource();
    void generateDefinition();
    QString definitionActionTitle(Document *doc, CodeEditor *editor) const; // "" when the caret is not on a declaration
    QAction *m_switchHeaderAct = nullptr, *m_generateDefAct = nullptr;

    // Java / Kotlin: Kotlin scripts (F5, kotlinc check), main-function run markers, package fix, dependencies, JDK,
    // version status, Gradle task timings (MainWindowJvm.cpp)
    void createJvmActions();
    void setupJvm();
    void wireJvm(Document *doc, CodeEditor *editor);
    void updateMainMarkers(Document *doc);
    void showMainMenu(Document *doc, int line);
    void runMain(Document *doc, const QString &mainClass, bool debug);
    // Runs (or debugs) the tests of a class, or one test when `method` is set, with the project's Gradle / Maven.
    void runTest(Document *doc, const QString &testClass, const QString &method, bool debug);
    bool runKotlinScript(Document *doc, bool debug); // false when the file is no Kotlin script
    void checkPackage(Document *doc);
    QList<QPair<QString, std::function<void()>>> localCodeActions(Document *doc, CodeEditor *editor);
    void showDependencies(bool updates);
    void showJdkManager();
    QString jdkStartupCommand(const QString &shell) const;
    void updateJvmStatus();
    // The Kotlin / Java server cannot import the Gradle build (no installed JDK runs its Gradle, or its toolchain is
    // missing): explains it once per project and session and offers a JDK download or a Gradle upgrade.
    void offerJdkFix(const QString &serverName);
    // Downloads Eclipse Temurin `major` into ~/.jdks (JdkInstaller) with a progress dialog; with `useForProject` it
    // becomes the project JDK and the Java / Kotlin servers restart with it.
    void downloadJdk(int major, bool useForProject);
    void projectJdkChanged(); // terminals offered a restart, Java / Kotlin servers restarted
    void restartJvmServers(); // the running Kotlin / Java servers, so they import the build again with the current JDK
    void runInTerminal(const QString &command, const QString &workDir = QString()); // Gradle commands get the task-timing script
    void applyDiagnostics(const QString &path); // language server + kotlinc script check
    QAction *m_debugAct = nullptr, *m_resumeAct = nullptr, *m_pauseAct = nullptr, *m_stepOverAct = nullptr, *m_stepIntoAct = nullptr;
    QAction *m_stepOutAct = nullptr, *m_stopDebugAct = nullptr, *m_breakpointAct = nullptr, *m_debugPanelAct = nullptr;
    QAction *m_dependenciesAct = nullptr, *m_depUpdatesAct = nullptr, *m_jdkAct = nullptr;
    ScriptCheck *m_scriptCheck = nullptr;
    QWidget *m_jvmStatus = nullptr;
    QToolButton *m_kotlinBtn = nullptr, *m_javaBtn = nullptr, *m_buildToolBtn = nullptr;
    QSet<QString> m_chmodAsked; // scripts whose missing exec bit was already offered this session
    QSet<QString> m_jdkFixOffered; // projects whose missing JDK was already explained this session

    // Java / Kotlin editing: Copy Reference, New Kotlin / Java class, Kotlin scratch files, completion of gradle.properties /
    // local.properties / gradle-wrapper.properties keys and of @file: annotations (MainWindowJvmEdit.cpp)
    void createJvmEditActions();
    void wireJvmEdit(Document *doc, CodeEditor *editor);
    void copyReference();
    void newJvmClass(const QString &dir, bool kotlin);
    void newScratchFile(const QString &kind); // "kts", "main.kts", "java"
    void fillScratchMenu();
    QString scratchDir() const;
    void scriptDependencySearch(CodeEditor *editor, const QString &query);
    QAction *m_copyRefAct = nullptr, *m_scratchAct = nullptr, *m_newKotlinAct = nullptr, *m_newJavaAct = nullptr;
    QMenu *m_scratchMenu = nullptr;
    MavenCentral *m_scriptCentral = nullptr;
    QTimer *m_scriptSearchTimer = nullptr;
    QPointer<CodeEditor> m_scriptSearchEditor;
    QString m_scriptSearchQuery;
    QHash<QString, QStringList> m_scriptSearchHits; // query (or "g:a:" for versions) -> "coordinate\tdetail"
    QSet<QString> m_scriptSearchPending;

    // Maven panel (MainWindowMaven.cpp)
    void setupMaven();
    MavenPanel *mavenPanel();
    void updateMavenUi();
    void toggleMavenPanel();
    MavenPanel *m_maven = nullptr;
    QAction *m_mavenAct = nullptr;

    // Game panel (MainWindowGame.cpp)
    GamePanel *gamePanel();
    void toggleGamePanel();
    GamePanel *m_gamePanel = nullptr;
    QAction *m_gameAct = nullptr;

    // Debugger (MainWindowDebug.cpp)
    void setupDebugger();
    DebugPanel *debugPanel();
    void startDebug();
    void launchDebug(const QString &command, const QString &target, const QString &workDir, int port);
    QString debugCommandFor(const QString &runCommand, int port) const; // a run command made to wait for the debugger
    // A Gradle init script that makes the program's JVMs (JavaExec), or with `tests` the test JVMs, wait for the debugger.
    QString debugInitScript(int port, bool tests = false) const;
    QList<DebugSourceBreakpoints> breakpointSpecs() const;
    void breakpointsEdited(Document *doc, CodeEditor *editor);
    void showDebugPanel();
    void clearExecutionLine();
    void updateDebugActions();
    QString resolveSource(const QString &pkgPath, const QString &sourceName) const;
    DebugSession *m_debug = nullptr;
    DebugPanel *m_debugPanel = nullptr;
    QHash<QString, QList<int>> m_breakpoints; // path -> 0-based lines (persisted)
    QString m_execPath;                       // the file showing the debugger's current line
    QString m_debugFile;                      // a single file being debugged outside the project
    int m_debugStops = 0;                     // how often the program stopped (the chat's debug tools wait for it to move)
    QString m_lastTestClass, m_lastTestFile;  // the last ▶ test run (Fix with QODE knows a failed test by it)
    QString m_debugEndReason;                 // why the last session ended
    // The chat's debugger tools (MainWindowIdeDebug.cpp).
    QString ideRelative(const QString &path) const;
    void setBreakpointLines(const QString &path, const QList<int> &zeroBased);
    QJsonObject ideBreakpoints(const QString &action, const QString &path, int line);
    QJsonObject ideDebugStart(const QString &path);
    QJsonObject ideDebugState(int frame, quint64 expand, bool brief = false); // brief: no frames / variables (polling)
    QJsonObject ideDebugControl(const QString &what);
    QJsonObject ideDebugEvaluate(const QString &expression, int frame);

    // Multiple cursors in the Edit menu, Ctrl+click links, local history (MainWindowEditing.cpp)
    void createEditingActions();
    void wireEditing(Document *doc, CodeEditor *editor);
    void requestLinks(Document *doc, CodeEditor *editor);
    void showLocalHistory(const QString &path); // a file, or a folder (its files, deleted ones included)
    QList<QAction *> m_cursorActs;
    QAction *m_localHistoryAct = nullptr;
    QAction *m_copyImageAct = nullptr; // "Copy as Image…" (CodeImageDialog)

    // Inline edit box: Ctrl+K, "/?", "Ask about this to make a change" (MainWindowInline.cpp)
    void createInlineActions();
    void wireInline(Document *doc, CodeEditor *editor);
    void openInlineEdit(Document *doc, CodeEditor *editor, int triggerPosition); // triggerPosition: where "/?" was typed, or -1
    QAction *m_inlineEditAct = nullptr, *m_askChangeAct = nullptr;

    // Language server assists: hints, highlights, light bulb, source actions (MainWindowAssist.cpp)
    void createAssistActions();
    void wireAssists(Document *doc, CodeEditor *editor);
    void applyAssistSettings();
    void requestHighlights(Document *doc, CodeEditor *editor);
    void updateLightBulb(Document *doc, CodeEditor *editor);
    void requestHints(Document *doc, CodeEditor *editor, int firstLine, int lastLine);
    void requestServerData(Document *doc, CodeEditor *editor); // semantic tokens, folding ranges, document symbols
    // Code lenses (MainWindowLens.cpp): the server's merged with QODE's own (Java / Kotlin tests and mains).
    void wireCodeLenses(Document *doc, CodeEditor *editor);
    void requestCodeLenses(Document *doc, CodeEditor *editor);
    QVector<LspCodeLens> localCodeLenses(Document *doc) const;
    void runCodeLens(Document *doc, CodeEditor *editor, const LspCodeLens &lens);
    void runSourceAction(const QStringList &kinds, const QString &what);
    void applyFileOps(const QVector<LspManager::FileOp> &ops, bool before);
    QAction *m_organizeImportsAct = nullptr, *m_removeUnusedAct = nullptr, *m_addMissingAct = nullptr, *m_fixAllAct = nullptr, *m_quickFixAct = nullptr;
    QAction *m_fixOnSaveAct = nullptr, *m_organizeOnSaveAct = nullptr, *m_hintParamsAct = nullptr, *m_hintTypesAct = nullptr;
    QAction *m_symbolHighlightAct = nullptr, *m_lightBulbAct = nullptr, *m_signatureAct = nullptr, *m_quickInfoAct = nullptr;
    QAction *m_semanticAct = nullptr, *m_typeDefAct = nullptr, *m_callHierarchyAct = nullptr, *m_typeHierarchyAct = nullptr, *m_codeLensAct = nullptr, *m_implAct = nullptr, *m_workspaceSymbolAct = nullptr;
    void requestColors(Document *doc, CodeEditor *editor);
    void pickColor(Document *doc, CodeEditor *editor, int index);
    Document *documentOf(CodeEditor *editor) const;

    // The chat's IDE tools: panels, theme, editor options and commands, language servers (MainWindowIde.cpp)
    void wireIdeTools();
    QJsonObject ideAction(const QString &action, const QJsonObject &args);
    QHash<QString, QAction *> ideOptionActions() const; // set_option names -> the checkable menu actions
    QJsonObject ideState() const;
    QJsonObject ideSetTheme(const QString &wanted);
    QJsonObject ideShowPanel(const QString &panel, bool visible);
    QJsonObject ideOpenFile(const QString &path, int line, const QString &where);
    QJsonObject ideSetOption(const QJsonObject &args);
    QJsonObject ideEditorCommand(const QString &command, const QString &path, int line);
    QJsonObject ideCheckServers(const QString &path);
    QJsonObject ideInstallServer(const QString &id);
    QJsonObject ideToolGroups() const;
    QJsonObject ideTerminalOutput(int tab, int lines);
    // The chat's http_request (MainWindowHttp.cpp): sends a request of a .http file through the HTTP panel; the chat polls.
    QJsonObject ideHttpSend(const QString &path, const QString &request, int line, const QString &environment);
    QJsonObject ideHttpPoll();
    void ideHttpStop();
    void noteHttpForChat(const HttpResult &r);
    bool m_httpForChat = false;
    int m_httpChatLine = -1;
    QJsonArray m_httpChatResults; // {line, label, json} of the requests the run sent
    // The chat's server tools (MainWindowIdeServers.cpp): every server QODE runs for the project, by id.
    struct IdeServer {
        QString id, name, folder, command, problem;
        DevServer *server = nullptr; // nullptr while it has not run (an Expo app the preview Metro does not serve)
        std::function<QString()> phase; // "Building" while a JVM server compiles
        std::function<void()> start, stop, restart;
    };
    QVector<IdeServer> ideServers();
    static const IdeServer *ideFindServer(const QVector<IdeServer> &servers, const QString &id, QString *err);
    static QJsonObject ideServerInfo(const IdeServer &s);
    QJsonObject ideServerStatus(const QString &id);
    QJsonObject ideServerControl(const QString &id, const QString &what);
    QJsonObject ideServerLog(const QString &id, int lines, const QString &grep);
    // The chat's preview tools (MainWindowIdePreview.cpp): the preview shown with its page loaded, or nullptr + `err`.
    WebPreview *idePreview(const QString &server, QString *err);
    QJsonObject idePreviewState(WebPreview *p) const;
    QJsonObject idePreviewAction(const QString &action, const QJsonObject &args);
    int m_previewConsoleRead = 0; // the serial of the last console entry preview_console returned

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
    QProgressBar *m_lspProgress = nullptr; // a server reports work in progress (import, indexing)
    QColor m_lspButtonColor;
    bool m_dying = false; // the editors are deleted after our members: their destroyed() slots must not touch them
    QPointer<ReferencesDialog> m_refsDialog;
    QPointer<CallHierarchyDialog> m_callDialog, m_typeDialog;
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
    MediaPanel *m_media = nullptr;
    MarkdownPreview *m_md = nullptr;
    GradlePanel *m_gradle = nullptr;
    SpringPanel *m_spring = nullptr;
    KtorPanel *m_ktor = nullptr;
    HttpPanel *m_http = nullptr;
    WebPreview *m_webPreview = nullptr;
    AiHub *m_ai = nullptr;
    QAction *m_aiAct = nullptr;
    QAction *m_claudeCodeAct = nullptr; // the chat area's Claude Code page
    QAction *m_mcpAct = nullptr;        // the QODE chat's MCP servers dialog
    QAction *m_editorPaneAct = nullptr;
    QSplitter *m_workSplit;        // the agent (centre, the main view) | the editor column on its right
    QWidget *m_agentIsland;
    QWidget *m_editorColumn;       // the editor island inside m_workSplit
    int m_editorColWidth = 0;      // width of the editor column beside the agent; 0 = not chosen yet
    bool m_editorHiddenByUser = false; // the editor column was closed by hand: files the agent opens do not bring it back
    bool m_launching = false;          // opening the startup paths / last session: the chat stays closed whatever the layout says
    QStackedWidget *m_rightStack; // media panel | markdown preview | Gradle tasks | Spring Boot | Ktor | web preview | HTTP client | game
    QPointer<Document> m_previewDoc; // the Markdown / SVG file the right panel is previewing live
    QMetaObject::Connection m_previewConn;
    QMetaObject::Connection m_previewScrollConn;
    bool m_scrollSync = false; // a scroll we caused ourselves while keeping editor and preview aligned
    QTimer *m_previewTimer;
    int m_mediaWidth = 420;
    int m_sideWidth = 250;
    QSplitter *m_vsplit; // m_workSplit above, the terminal below
    int m_terminalHeight = 240;

    QAction *m_newWindowAct, *m_newProjectAct, *m_openProjectAct, *m_closeProjectAct, *m_exitAct;
    QAction *m_newFileAct, *m_openFileAct, *m_saveAct, *m_saveAsAct, *m_saveAllAct, *m_closeFileAct;
    QAction *m_undoAct, *m_redoAct, *m_cutAct, *m_copyAct, *m_pasteAct, *m_selectAllAct, *m_findAct, *m_replaceAct;
    QAction *m_projNewFileAct, *m_projNewFolderAct, *m_openProjectFolderAct;
    QPointer<QDialog> m_themeEditor;
    void showThemeEditor();
    QAction *m_explorerAct, *m_terminalAct, *m_runAct, *m_runConfigAct, *m_pythonPkgAct, *m_gradleAct, *m_venvAct, *m_fullscreenAct, *m_wordWrapAct, *m_zoomInAct, *m_zoomOutAct, *m_zoomResetAct;
    QAction *m_nextTabAct, *m_prevTabAct, *m_splitRightAct, *m_splitDownAct, *m_aboutAct, *m_paletteAct, *m_quickOpenAct, *m_gotoLineAct, *m_gotoSymbolAct, *m_gotoDefinitionAct, *m_renameAct, *m_findRefsAct, *m_searchAct, *m_matchBracketAct, *m_indentGuidesAct, *m_stickyAct, *m_predictAct, *m_blameInlineAct, *m_bookmarkToggleAct, *m_bookmarkNextAct, *m_bookmarkPrevAct, *m_showBookmarksAct, *m_showTodosAct, *m_previewAct, *m_newTerminalAct, *m_blameGutterAct, *m_minimapAct, *m_breadcrumbsAct, *m_hiddenFilesAct;
    QMenu *m_recentMenu;
    DevServerBar *m_serverBar = nullptr;
    ExpoBar *m_expoBar = nullptr;
    QAction *m_expoBarAct = nullptr;
    ExpoSchema *m_expoSchema = nullptr;
    QPointer<ExpoConfigDialog> m_expoDialog;
    bool m_metroStarted = false; // an `expo start` was launched from the toolbar this session (DevTools needs it)
    QAction *m_serverBarAct = nullptr;
    SpringBar *m_springBar = nullptr;
    QAction *m_springBarAct = nullptr;
    QAction *m_springAct = nullptr;
    QAction *m_springStarterAct = nullptr;
    KtorBar *m_ktorBar = nullptr;
    QAction *m_ktorBarAct = nullptr;
    QAction *m_ktorAct = nullptr;
    QAction *m_ktorPluginAct = nullptr;
    QAction *m_httpAct = nullptr;
    QAction *m_newHttpAct = nullptr;
    QToolBar *m_mainToolBar = nullptr;
    QAction *m_autoSaveOffAct, *m_autoSaveDelayAct, *m_autoSaveFocusAct, *m_autoSaveDelayMsAct, *m_trimAct, *m_finalNewlineAct, *m_formatOnSaveAct, *m_formatAct;
    QAction *m_foldAct, *m_unfoldAct, *m_foldAllAct, *m_unfoldAllAct;

    QAction *m_scmAct, *m_gitRefreshAct, *m_gitFetchAct, *m_gitPullAct, *m_gitPushAct, *m_gitInitAct, *m_gitNewBranchAct;
    QAction *m_gitStageFileAct, *m_gitUnstageFileAct, *m_gitDiscardFileAct, *m_gitDiffFileAct, *m_nextChangeAct, *m_prevChangeAct;
    QWidget *m_gitStatusWidget;
    BranchButton *m_branchButton;
    QToolButton *m_aheadBtn, *m_behindBtn;
    QLabel *m_changesPill;
    QString m_lastHead, m_lastRoot;
    QHash<QString, QPointer<DiffDialog>> m_diffs;

    QToolButton *m_zoomButton = nullptr;
    void updateZoomButton();
    QLabel *m_fileLabel, *m_langLabel, *m_encLabel, *m_eolLabel, *m_posLabel, *m_modeLabel;
};
