#include "app/InstanceRegistry.h"
#include "platform/Platform.h"
#include "MainWindow.h"

#include "Island.h"
#include "SideSections.h"
#include "dialogs/CompilerFlagsDialog.h"
#include "dialogs/IncludePathsDialog.h"
#include "dialogs/JdtlsInstallDialog.h"
#include "dialogs/LspInstallDialog.h"
#include "dialogs/LspLogDialog.h"
#include "dialogs/LspRemoveDialog.h"
#include "expo/ExpoBar.h"
#include "expo/ExpoConfigDialog.h"
#include "expo/ExpoSchema.h"
#include "dialogs/NpmInstallDialog.h"
#include "dialogs/PackagesDialog.h"
#include "dialogs/PipInstallDialog.h"
#include "dialogs/NewProjectDialog.h"
#include "dialogs/RunConfigDialog.h"
#include "webdev/DevServerBar.h"
#include "project/JvmProject.h"
#include "project/ProjectTemplates.h"
#include "project/VersionCatalog.h"
#include "project/QmakeProject.h"
#include "media/MarkdownPreview.h"
#include "tasks/TasksPanel.h"
#include "git/BranchButton.h"
#include "git/BranchPopup.h"
#include "git/DiffDialog.h"
#include "git/GitDiff.h"
#include "git/GitPanel.h"
#include "git/GitRepository.h"
#include "git/PatchDialog.h"
#include "dialogs/ReferencesDialog.h"
#include "dialogs/UnusedImportsDialog.h"
#include "lsp/JarSource.h"
#include "gradle/GradlePanel.h"
#include "gradle/GradleTasks.h"
#include "lsp/LspInstaller.h"
#include "lsp/LspServers.h"
#include "lsp/LspManager.h"
#include "palette/PalettePopup.h"
#include "editor/CodeEditor.h"
#include "editor/Breadcrumbs.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/FileIcons.h"
#include "explorer/ProjectExplorer.h"
#include "refactor/MoveController.h"
#include "filesystem/FileManager.h"
#include "media/MediaPanel.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "project/PythonEnv.h"
#include "python/PythonTools.h"
#include "search/ProjectSearch.h"
#include "search/SearchPanel.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "settings/ThemeManager.h"
#include "dialogs/ThemeEditorDialog.h"
#include "terminal/TerminalPanel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QWidgetAction>
#include <QMimeData>
#include <QMenu>
#include <QPointer>
#include <QUrl>
#include <QRegularExpression>
#include <QTextCursor>
#include <QStandardPaths>
#include <QSaveFile>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QJsonArray>
#include <QJsonObject>
#include <QTabBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <algorithm>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setAcceptDrops(true);
    m_projects = new ProjectManager(this);
    m_projectFiles = new ProjectFiles(this);
    m_git = new GitRepository(this);
    m_explorer = new ProjectExplorer(this);
    m_explorer->setGitRepository(m_git);
    m_gitPanel = new GitPanel(m_git, this);
    m_searchPanel = new SearchPanel(this);
    m_tasks = new TasksPanel(this);
    m_editors = new EditorManager(this);

    // Left side: collapsible Explorer / Version Control / Search / Tasks sections
    m_side = new SideSections(this);
    auto hideTitle = [](QWidget *w) { // the section header replaces the panel's own heading
        for (QLabel *l : w->findChildren<QLabel *>(QStringLiteral("panelTitle"), Qt::FindDirectChildrenOnly))
            l->hide();
    };
    for (QWidget *w : {static_cast<QWidget *>(m_explorer), static_cast<QWidget *>(m_searchPanel), static_cast<QWidget *>(m_tasks)})
        hideTitle(w);
    m_side->addSection(tr("Explorer"), m_explorer);
    m_side->addSection(tr("Version Control"), m_gitPanel);
    m_side->addSection(tr("Search"), m_searchPanel);
    m_side->addSection(tr("Tasks"), m_tasks);
    m_side->setExpandedStates(SettingsManager::instance().sideSections());
    connect(m_side, &SideSections::expansionChanged, this, [this] { SettingsManager::instance().setSideSections(m_side->expandedStates()); });

    m_terminal = new TerminalPanel(this);
    // Python projects: activate the project's virtualenv (whatever its folder is called) in fresh shells.
    m_terminal->setStartupCommandProvider([this](const QString &shell) {
        if (!m_projects->hasProject() || !SettingsManager::instance().autoActivateVenv())
            return QString();
        return PythonEnv::activationCommand(m_projects->project().root, shell);
    });

    connect(m_terminal, &TerminalPanel::tabsChanged, this, [](const QStringList &names) { SettingsManager::instance().setTerminalTabs(names); });

    m_vsplit = new QSplitter(Qt::Vertical, this);
    m_vsplit->setChildrenCollapsible(false);
    m_vsplit->setHandleWidth(8);
    auto *editorIsland = new Island(m_editors);
    editorIsland->setMinimumHeight(60); // otherwise the editor's own minimum stops the terminal from growing tall
    m_vsplit->addWidget(editorIsland);
    m_vsplit->addWidget(new Island(m_terminal));
    m_vsplit->setStretchFactor(0, 1);
    m_vsplit->setStretchFactor(1, 0);
    m_vsplit->widget(1)->hide(); // hidden until Ctrl+J
    connect(m_terminal, &TerminalPanel::maximizeToggled, this, [this](bool maximized) {
        const int total = m_vsplit->height();
        if (maximized) {
            m_terminalHeight = m_vsplit->sizes().value(1, m_terminalHeight);
            m_vsplit->setSizes({60, qMax(80, total - 60)});
        } else {
            const int h = qBound(80, m_terminalHeight, qMax(80, total - 100));
            m_vsplit->setSizes({total - h, h});
        }
    });

    m_hsplit = new QSplitter(Qt::Horizontal, this);
    m_hsplit->setChildrenCollapsible(false);
    m_hsplit->setHandleWidth(8);
    m_hsplit->setContentsMargins(8, 2, 8, 0);
    m_hsplit->addWidget(new Island(m_side));
    m_hsplit->addWidget(m_vsplit);
    m_media = new MediaPanel(this);
    m_md = new MarkdownPreview(this);
    m_rightStack = new QStackedWidget(this);
    m_rightStack->addWidget(m_media);
    m_rightStack->addWidget(m_md);
    m_gradle = new GradlePanel(this);
    m_rightStack->addWidget(m_gradle);
    connect(m_gradle, &GradlePanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_gradle, &GradlePanel::runRequested, this, [this](const QString &command, const QString &dir) {
        if (!m_editors->saveAll())
            return;
        QString quoted = dir;
        quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        showTerminal();
        m_terminal->runCommand(QStringLiteral("(cd '%1' && %2)").arg(quoted, command));
        m_terminal->focusTerminal();
    });
    m_hsplit->addWidget(new Island(m_rightStack));
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(250);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::updatePreview);
    m_hsplit->setStretchFactor(0, 0);
    m_hsplit->setStretchFactor(1, 1);
    m_hsplit->setStretchFactor(2, 0);
    m_hsplit->widget(2)->hide(); // shown when an image or video is opened
    setCentralWidget(m_hsplit);

    m_lsp = new LspManager(this);
    m_editors->setLspFormatter([this](Document *doc, CodeEditor *ed) {
        auto &cfg = SettingsManager::instance();
        QVector<LspTextEdit> edits;
        if (!m_lsp->formatting(doc, cfg.tabSize(), cfg.useSpaces(), 3000, &edits))
            return false;
        if (!edits.isEmpty())
            ed->applyTextEdits(edits);
        return true;
    });
    m_lsp->setApplyEditHandler([this](const QJsonObject &edit) { return applyWorkspaceEdit(edit); });
    createActions();
    createMenus();
    createToolBar();
    createStatusBar();

    connect(m_projects, &ProjectManager::projectOpened, this, &MainWindow::onProjectOpened);
    connect(m_projects, &ProjectManager::projectClosed, this, &MainWindow::onProjectClosed);
    connect(m_explorer, &ProjectExplorer::closeProjectRequested, this, &MainWindow::closeProject);
    connect(m_explorer, &ProjectExplorer::fileActivated, m_editors, &EditorManager::openFile);
    connect(m_explorer, &ProjectExplorer::fileCreated, m_editors, &EditorManager::openFile);
    m_mover = new MoveController(m_explorer, m_editors, this);
    connect(m_explorer, &ProjectExplorer::moveRequested, m_mover, &MoveController::move);
    connect(m_explorer, &ProjectExplorer::pathRenamed, m_editors, &EditorManager::pathRenamed);
    connect(m_explorer, &ProjectExplorer::pathDeleted, m_editors, &EditorManager::closeDocumentsUnder);
    connect(m_explorer, &ProjectExplorer::contentsChanged, m_projectFiles, &ProjectFiles::invalidate);
    connect(m_editors, &EditorManager::documentPathChanged, m_projectFiles, &ProjectFiles::invalidate);

    connect(m_editors, &EditorManager::mediaRequested, this, &MainWindow::showMedia);
    connect(m_media, &MediaPanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_md, &MarkdownPreview::closeRequested, this, &MainWindow::hideMedia);
    connect(m_md, &MarkdownPreview::openFileRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_editors, &EditorManager::previewRequested, this, &MainWindow::togglePreview);
    connect(m_editors, &EditorManager::currentChanged, this, &MainWindow::followPreview);
    connect(m_explorer, &ProjectExplorer::pathDeleted, this, [this](const QString &path) {
        const QString cur = m_media->currentPath();
        if (!cur.isEmpty() && (cur == path || cur.startsWith(path + QLatin1Char('/'))))
            hideMedia();
    });
    connect(m_explorer, &ProjectExplorer::pathRenamed, this, [this](const QString &from, const QString &to) {
        const QString cur = m_media->currentPath();
        if (cur == from)
            m_media->openMedia(to);
        else if (cur.startsWith(from + QLatin1Char('/')))
            m_media->openMedia(to + cur.mid(from.size()));
    });
    connect(m_editors, &EditorManager::newProjectRequested, m_newProjectAct, &QAction::trigger);
    connect(m_editors, &EditorManager::openProjectRequested, m_openProjectAct, &QAction::trigger);
    connect(m_editors, &EditorManager::newFileRequested, m_newFileAct, &QAction::trigger);
    connect(m_editors, &EditorManager::openFileRequested, m_openFileAct, &QAction::trigger);
    connect(m_editors, &EditorManager::openRecentProjectRequested, this, &MainWindow::openRecentProject);
    connect(m_editors, &EditorManager::removeRecentProjectRequested, &SettingsManager::instance(), &SettingsManager::removeRecentProject);
    connect(m_editors, &EditorManager::clearRecentProjectsRequested, &SettingsManager::instance(), &SettingsManager::clearRecentProjects);
    connect(&SettingsManager::instance(), &SettingsManager::recentProjectsChanged, this, &MainWindow::refreshRecentProjects);
    refreshRecentProjects();

    m_searchPanel->setOverridesProvider([this] {
        QHash<QString, QString> out;
        for (Document *d : m_editors->modifiedDocuments())
            if (!d->isUntitled())
                out.insert(d->filePath(), d->text());
        return out;
    });
    connect(m_searchPanel, &SearchPanel::openMatch, this, [this](const QString &path, int line, int column, int length) {
        if (line > 0)
            m_editors->openFileAt(path, line, column + 1, length);
        else
            m_editors->openFile(path);
    });
    connect(m_searchPanel, &SearchPanel::replaceRequested, this, &MainWindow::replaceInFiles);

    m_tasks->setOverridesProvider([this] {
        QHash<QString, QString> out;
        for (Document *d : m_editors->modifiedDocuments())
            if (!d->isUntitled())
                out.insert(d->filePath(), d->text());
        return out;
    });
    m_tasks->setLineTextProvider([this](const QString &path, int line) { return lineTextOf(path, line); });
    m_tasks->setBookmarks(m_bookmarks);
    connect(m_tasks, &TasksPanel::openLocation, this, [this](const QString &path, int line, int column, int length) {
        if (line > 0)
            m_editors->openFileAt(path, line, column + 1, length);
        else
            m_editors->openFile(path);
    });
    connect(m_tasks, &TasksPanel::removeBookmark, this, [this](const QString &path, int line) {
        QList<int> lines = m_bookmarks.value(path);
        lines.removeAll(line);
        applyBookmarks(path, lines);
    });
    connect(m_tasks, &TasksPanel::clearBookmarksRequested, this, [this] {
        const QStringList paths = m_bookmarks.keys();
        for (const QString &p : paths)
            applyBookmarks(p, {});
    });
    connect(m_editors, &EditorManager::documentAdded, this, [this](Document *doc) {
        CodeEditor *ed = m_editors->editorFor(doc);
        if (!ed)
            return;
        if (!doc->filePath().isEmpty())
            ed->setBookmarks(m_bookmarks.value(doc->filePath()));
        if (!doc->filePath().isEmpty()) {
            if (SettingsManager::instance().importsFolded(doc->filePath()))
                ed->setImportsFolded(true);
            connect(ed, &CodeEditor::importsFoldedChanged, this, [doc](bool folded) {
                if (!doc->isUntitled())
                    SettingsManager::instance().setImportsFolded(doc->filePath(), folded);
            });
        }
        connect(ed, &CodeEditor::hoverRequested, this, [this, doc, ed](int line, int column) {
            QPointer<CodeEditor> guard(ed);
            m_lsp->hover(doc, line, column, [guard](const QString &text) {
                if (guard)
                    guard->showHover(text);
            });
        });
        connect(ed, &CodeEditor::completionRequested, this,
                [this, doc, ed](int line, int column, int kind, const QString &trigger, int token) {
                    QPointer<CodeEditor> guard(ed);
                    m_lsp->completion(doc, line, column, kind, trigger, [guard, token](const QVector<LspCompletionItem> &items, bool incomplete) {
                        if (guard)
                            guard->showCompletions(items, incomplete, token);
                    });
                });
        ed->setDefinitionAvailable([this, doc] { return m_lsp->isServed(doc); });
        if (VersionCatalog::isBuildScript(doc->filePath())) // libs.<alias> accessors from gradle/libs.versions.toml
            ed->setLocalCompletions([doc](const QString &beforeWord) {
                QVector<LspCompletionItem> items;
                for (const VersionCatalog::Entry &e : VersionCatalog::complete(doc->filePath(), beforeWord)) {
                    LspCompletionItem it;
                    it.label = e.label;
                    it.detail = e.detail;
                    it.kind = e.group ? 9 : 6;
                    items << it;
                }
                return items;
            });
        if (JarSource::isLibraryPath(doc->filePath()))
            ed->setReadOnly(true); // library source unpacked by Go to Definition
        // Kotlin completions add their import (and insert the text) through a server command.
        ed->setCompletionCommandRunner([this, doc](const LspCompletionItem &item, int line, int column, std::function<void(bool)> finished) {
            // Only Kotlin's command inserts the text; other servers (jdtls: java.completion.onDidSelect) attach commands
            // that are notifications, and running them instead of inserting would drop the completion.
            if (item.command != QLatin1String("jetbrains.kotlin.completion.apply") || !m_lsp->supportsCommand(doc, item.command))
                return false;
            m_lsp->runCompletionCommand(doc, item, line, column, std::move(finished));
            return true;
        });
        // Java completions get their import when the item is resolved.
        ed->setCompletionResolver([this, doc](const LspCompletionItem &item, std::function<void(const LspCompletionItem &)> finished) {
            m_lsp->resolveCompletion(doc, item, std::move(finished));
        });
        connect(ed, &CodeEditor::removeUnusedImportsRequested, this, [this, ed] { removeUnusedImports(ed); });
        connect(ed, &CodeEditor::colorsRequested, this, [this, doc, ed] { requestColors(doc, ed); });
        connect(ed, &CodeEditor::swatchClicked, this, [this, doc, ed](int index) { pickColor(doc, ed, index); });
        connect(ed, &QObject::destroyed, this, [this, ed, doc] {
            m_colorData.remove(ed);
            m_colorAsked.remove(doc);
        });
        connect(ed, &CodeEditor::codeActionsRequested, this, [this, doc, ed](int sl, int sc, int el, int ec) { showCodeActions(doc, ed, sl, sc, el, ec); });
        connect(ed, &CodeEditor::definitionRequested, this, [this, ed](int line, int column) { goToDefinition(ed, line, column); });
        connect(ed, &CodeEditor::bookmarksChanged, this, [this, doc, ed] {
            if (!doc->isUntitled())
                applyBookmarks(doc->filePath(), ed->bookmarks());
        });
    });
    connect(m_editors, &EditorManager::documentSaved, this, [this] {
        m_tasks->setBookmarks(m_bookmarks); // refreshes the quoted line text
        m_tasks->scheduleTodoScan();
    });

    connect(m_editors, &EditorManager::documentAdded, m_lsp, &LspManager::documentOpened);
    connect(m_editors, &EditorManager::documentSaved, m_lsp, &LspManager::documentSaved);
    connect(m_editors, &EditorManager::documentSaved, this, [this](Document *doc) {
        const bool jvmBuild = LspManager::isJvmBuildFile(doc->filePath()) && m_lsp->canSyncBuildFiles();
        m_lsp->buildFileSaved(doc->filePath());
        if (jvmBuild)
            statusBar()->showMessage(tr("%1 saved — syncing the project…").arg(QFileInfo(doc->filePath()).fileName()), 5000);
    });
    connect(m_lsp, &LspManager::buildSyncFinished, this, [this](const QString &server) {
        statusBar()->showMessage(tr("✓ Build files synced (%1)").arg(server), 6000);
    });
    connect(m_editors, &EditorManager::documentPathChanged, m_lsp, &LspManager::documentPathChanged);
    connect(m_lsp, &LspManager::statusChanged, this, &MainWindow::updateLspStatus);
    // A server that just finished starting: fetch the colour literals of the files already open.
    auto *colorRetry = new QTimer(this);
    colorRetry->setSingleShot(true);
    colorRetry->setInterval(700);
    connect(colorRetry, &QTimer::timeout, this, [this] {
        for (Document *d : m_editors->documents())
            if (CodeEditor *ed = m_editors->editorFor(d))
                if (!ed->hasColorSwatches() && m_lsp->isServed(d))
                    requestColors(d, ed);
    });
    connect(m_lsp, &LspManager::statusChanged, colorRetry, qOverload<>(&QTimer::start));
    connect(m_lsp, &LspManager::diagnosticsChanged, this, [this](const QString &path) {
        if (Document *d = m_editors->documentForPath(path))
            if (CodeEditor *ed = m_editors->editorFor(d)) {
                ed->setDiagnostics(m_lsp->diagnostics(path));
                m_importPath = path;
                m_importTimer->start(); // settles after the diagnostics stop changing
            }
    });
    m_importTimer = new QTimer(this);
    m_importTimer->setSingleShot(true);
    m_importTimer->setInterval(1200);
    connect(m_importTimer, &QTimer::timeout, this, [this] {
        Document *d = m_editors->documentForPath(m_importPath);
        CodeEditor *ed = d ? m_editors->editorFor(d) : nullptr;
        if (!ed || !(d->text().startsWith(QLatin1String("import ")) || d->text().contains(QLatin1String("\nimport "))) || !m_lsp->isServed(d))
            return;
        QPointer<CodeEditor> guard(ed);
        m_lsp->unusedImports(d, [guard](const QVector<int> &lines) {
            if (guard)
                guard->setUnusedImports(lines);
        });
    });
    updateLspStatus();

    connect(m_terminal, &TerminalPanel::hideRequested, this, &MainWindow::toggleTerminal);
    connect(m_editors, &EditorManager::currentChanged, this, [this] { updateStatus(); updateActions(); updateTitle(); noteRecentFile(); });
    connect(m_editors, &EditorManager::documentStateChanged, this, [this] { updateStatus(); updateActions(); updateTitle(); });
    connect(m_editors, &EditorManager::cursorInfoChanged, this, &MainWindow::updateStatus);

    setupGit();
    restoreSettings();
    onProjectClosed();
    updateStatus();
    updateActions();
}

void MainWindow::createActions()
{
    auto make = [this](const QString &text, const QKeySequence &key = {}, const QString &icon = {}) {
        auto *a = new QAction(text, this);
        if (!key.isEmpty())
            a->setShortcut(key);
        if (!icon.isEmpty())
            Icons::bind(a, icon);
        return a;
    };
    using K = Qt::Key;
    const auto C = Qt::CTRL;
    const auto S = Qt::SHIFT;
    const auto A = Qt::ALT;

    m_newWindowAct = make(tr("New Window"), QKeySequence(C | A | K::Key_N));
    m_newProjectAct = make(tr("New Project…"), QKeySequence(C | S | K::Key_N));
    m_openProjectAct = make(tr("Open Project…"), QKeySequence(C | S | K::Key_O), QStringLiteral(":/new-icons/folder.svg"));
    m_closeProjectAct = make(tr("Close Project"), QKeySequence(C | S | K::Key_W), QStringLiteral(":/new-icons/x.svg"));
    m_exitAct = make(tr("Exit"), QKeySequence(C | K::Key_Q));

    m_newFileAct = make(tr("New File…"), QKeySequence(C | K::Key_N), QStringLiteral(":/new-icons/file-plus.svg"));
    m_openFileAct = make(tr("Open File…"), QKeySequence(C | K::Key_O), QStringLiteral(":/new-icons/folder-open.svg"));
    m_saveAct = make(tr("Save"), QKeySequence(C | K::Key_S), QStringLiteral(":/new-icons/save.svg"));
    m_saveAsAct = make(tr("Save As…"), QKeySequence(C | S | K::Key_S));
    m_saveAllAct = make(tr("Save All"), QKeySequence(C | S | A | K::Key_S));
    m_closeFileAct = make(tr("Close File"), QKeySequence(C | K::Key_W));

    m_undoAct = make(tr("Undo"), QKeySequence(C | K::Key_Z));
    m_redoAct = make(tr("Redo"), QKeySequence(C | S | K::Key_Z));
    m_cutAct = make(tr("Cut"), QKeySequence(C | K::Key_X));
    m_copyAct = make(tr("Copy"), QKeySequence(C | K::Key_C));
    m_pasteAct = make(tr("Paste"), QKeySequence(C | K::Key_V));
    m_selectAllAct = make(tr("Select All"), QKeySequence(C | K::Key_A));
    m_findAct = make(tr("Find"), QKeySequence(C | K::Key_F), QStringLiteral(":/new-icons/search.svg"));
    m_replaceAct = make(tr("Replace"), QKeySequence(C | K::Key_H));

    m_projNewFileAct = make(tr("New File…"), {}, QStringLiteral(":/new-icons/file-plus.svg"));
    m_projNewFolderAct = make(tr("New Folder…"), {}, QStringLiteral(":/new-icons/folder-plus.svg"));
    m_openProjectFolderAct = make(tr("Open Project Folder…"));

    m_explorerAct = make(tr("Toggle Sidebar"), QKeySequence(C | K::Key_B), QStringLiteral(":/new-icons/panel-left.svg"));
    m_explorerAct->setToolTip(tr("Show or hide the left panel (Ctrl+B)"));
    m_explorerAct->setCheckable(true);
    m_explorerAct->setChecked(true);
    m_terminalAct = make(tr("Terminal"), QKeySequence(C | K::Key_J), QStringLiteral(":/new-icons/terminal.svg"));
    m_terminalAct->setCheckable(true);
    m_venvAct = make(tr("Auto-Activate Python venv"));
    m_venvAct->setCheckable(true);
    m_venvAct->setChecked(SettingsManager::instance().autoActivateVenv());
    connect(m_venvAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setAutoActivateVenv(on); });
    m_runAct = make(tr("Run File"), QKeySequence(K::Key_F5));
    m_runAct->setToolTip(tr("Run the project, or this file (F5)"));
    m_runConfigAct = make(tr("Run Configuration…"), {}, QStringLiteral(":/new-icons/cog.svg"));
    m_runConfigAct->setToolTip(tr("Set the command that runs the project or this type of file"));
    m_gradleAct = make(tr("Gradle Tasks"), {}, QStringLiteral(":/new-icons/gradle.svg"));
    m_gradleAct->setToolTip(tr("Show the Gradle tasks of this project (build, test, package, …) and run them"));
    m_pythonPkgAct = make(tr("Python Packages…"), {}, QStringLiteral(":/new-icons/package.svg"));
    m_pythonPkgAct->setToolTip(tr("Install, upgrade and remove the packages of the project's virtual environment"));
    {
        // Green outlined play button; recoloured with the theme since green must differ on light/dark.
        auto paint = [this] {
            m_runAct->setIcon(Icons::tinted(QStringLiteral(":/new-icons/play.svg"),
                                            Theme::byName(SettingsManager::instance().theme()).success));
        };
        paint();
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, m_runAct, paint);
    }
    m_fullscreenAct = make(tr("Toggle Fullscreen"), QKeySequence(K::Key_F11));
    m_wordWrapAct = make(tr("Word Wrap"), QKeySequence(A | K::Key_Z));
    m_wordWrapAct->setCheckable(true);
    m_wordWrapAct->setChecked(SettingsManager::instance().wordWrap());
    m_breadcrumbsAct = make(tr("Breadcrumbs"));
    m_breadcrumbsAct->setCheckable(true);
    m_breadcrumbsAct->setChecked(SettingsManager::instance().showBreadcrumbs());
    m_minimapAct = make(tr("Minimap"));
    m_minimapAct->setCheckable(true);
    m_minimapAct->setChecked(SettingsManager::instance().showMinimap());
    m_hiddenFilesAct = make(tr("Show Hidden Files in Explorer"));
    m_hiddenFilesAct->setCheckable(true);
    m_hiddenFilesAct->setChecked(SettingsManager::instance().showHiddenFiles());
    m_blameInlineAct = make(tr("Inline Blame on Current Line"));
    m_blameInlineAct->setCheckable(true);
    m_blameInlineAct->setChecked(SettingsManager::instance().blameInline());
    m_blameGutterAct = make(tr("Blame Column in Gutter"));
    m_blameGutterAct->setCheckable(true);
    m_blameGutterAct->setChecked(SettingsManager::instance().blameGutter());
    m_stickyAct = make(tr("Sticky Scroll"));
    m_stickyAct->setCheckable(true);
    m_stickyAct->setChecked(SettingsManager::instance().stickyScroll());
    m_indentGuidesAct = make(tr("Indent Guides"));
    m_indentGuidesAct->setCheckable(true);
    m_indentGuidesAct->setChecked(SettingsManager::instance().indentGuides());
    m_nextTabAct = make(tr("Next Tab"), QKeySequence(C | K::Key_Tab));
    m_prevTabAct = make(tr("Previous Tab"), QKeySequence(C | S | K::Key_Backtab));
    m_splitRightAct = make(tr("Split Editor Right"), QKeySequence(C | K::Key_Backslash));
    m_splitDownAct = make(tr("Split Editor Down"), QKeySequence(C | Qt::ALT | K::Key_Backslash));
    m_aboutAct = make(tr("About QODE"));
    m_paletteAct = make(tr("Command Palette…"), QKeySequence(C | S | K::Key_P), QStringLiteral(":/new-icons/search.svg"));
    m_quickOpenAct = make(tr("Go to File…"), QKeySequence(C | K::Key_P), QStringLiteral(":/new-icons/file-input.svg"));
    m_gotoDefinitionAct = make(tr("Go to Definition"), QKeySequence(K::Key_F12));
    m_gotoLineAct = make(tr("Go to Line…"), QKeySequence(C | K::Key_G));
    m_bookmarkToggleAct = make(tr("Toggle Bookmark"), QKeySequence(C | K::Key_F2));
    m_bookmarkNextAct = make(tr("Next Bookmark"), QKeySequence(S | K::Key_F2));
    m_bookmarkPrevAct = make(tr("Previous Bookmark"), QKeySequence(C | S | K::Key_F2));
    m_renameAct = make(tr("Rename Symbol…"), QKeySequence(K::Key_F2));
    m_findRefsAct = make(tr("Find All References"), QKeySequence(S | K::Key_F12));
    m_newTerminalAct = make(tr("New Terminal"), QKeySequence(C | S | K::Key_T), QStringLiteral(":/new-icons/plus.svg"));
    m_previewAct = make(tr("Toggle Preview (Markdown / SVG)"), QKeySequence(C | S | K::Key_V));
    m_showBookmarksAct = make(tr("Show Bookmarks"));
    m_showTodosAct = make(tr("Show TODO Comments"));
    m_gotoSymbolAct = make(tr("Go to Symbol…"), QKeySequence(C | K::Key_R));
    m_matchBracketAct = make(tr("Go to Matching Bracket"), QKeySequence(C | S | K::Key_Backslash));
    auto &cfg = SettingsManager::instance();
    m_autoSaveOffAct = make(tr("Off"));
    m_autoSaveDelayAct = make(tr("After Delay"));
    m_autoSaveFocusAct = make(tr("On Focus Change"));
    m_autoSaveDelayMsAct = make(tr("Auto Save Delay…"));
    auto *autoGroup = new QActionGroup(this);
    for (QAction *a : {m_autoSaveOffAct, m_autoSaveDelayAct, m_autoSaveFocusAct}) {
        a->setCheckable(true);
        autoGroup->addAction(a);
    }
    (cfg.autoSaveMode() == SettingsManager::AutoSaveAfterDelay ? m_autoSaveDelayAct
     : cfg.autoSaveMode() == SettingsManager::AutoSaveOnFocusChange ? m_autoSaveFocusAct
                                                                     : m_autoSaveOffAct)->setChecked(true);
    m_trimAct = make(tr("Trim Trailing Whitespace"));
    m_trimAct->setCheckable(true);
    m_trimAct->setChecked(cfg.trimTrailingWhitespace());
    m_finalNewlineAct = make(tr("Insert Final Newline"));
    m_finalNewlineAct->setCheckable(true);
    m_finalNewlineAct->setChecked(cfg.insertFinalNewline());
    m_formatOnSaveAct = make(tr("Format on Save"));
    m_formatOnSaveAct->setCheckable(true);
    m_formatOnSaveAct->setChecked(cfg.formatOnSave());
    m_formatAct = make(tr("Format Document"), QKeySequence(A | S | K::Key_F));
    connect(m_autoSaveOffAct, &QAction::triggered, this, [] { SettingsManager::instance().setAutoSaveMode(SettingsManager::AutoSaveOff); });
    connect(m_autoSaveDelayAct, &QAction::triggered, this, [] { SettingsManager::instance().setAutoSaveMode(SettingsManager::AutoSaveAfterDelay); });
    connect(m_autoSaveFocusAct, &QAction::triggered, this, [] { SettingsManager::instance().setAutoSaveMode(SettingsManager::AutoSaveOnFocusChange); });
    connect(m_autoSaveDelayMsAct, &QAction::triggered, this, [this] {
        auto &s = SettingsManager::instance();
        bool ok = false;
        const int ms = QInputDialog::getInt(this, tr("Auto Save Delay"), tr("Save after typing stops for (milliseconds, 100–2000):"),
                                            s.autoSaveDelayMs(), 100, 2000, 50, &ok);
        if (ok)
            s.setAutoSaveDelayMs(ms);
    });
    connect(m_trimAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setTrimTrailingWhitespace(on); });
    connect(m_finalNewlineAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setInsertFinalNewline(on); });
    connect(m_formatOnSaveAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setFormatOnSave(on); });
    // Format on Save and venv activation can be overridden per project; resync the menu on project change.
    connect(&SettingsManager::instance(), &SettingsManager::projectSettingsChanged, this, [this] {
        auto &s = SettingsManager::instance();
        QSignalBlocker b1(m_formatOnSaveAct), b2(m_venvAct);
        m_formatOnSaveAct->setChecked(s.formatOnSave());
        m_venvAct->setChecked(s.autoActivateVenv());
    });
    connect(m_formatAct, &QAction::triggered, m_editors, &EditorManager::formatCurrent);
    connect(m_editors, &EditorManager::statusMessage, this, [this](const QString &t) { statusBar()->showMessage(t, 6000); });

    m_foldAct = make(tr("Fold"), QKeySequence(C | S | K::Key_BracketLeft));
    m_unfoldAct = make(tr("Unfold"), QKeySequence(C | S | K::Key_BracketRight));
    m_foldAllAct = make(tr("Fold All"), QKeySequence(QStringLiteral("Ctrl+K, Ctrl+0")));
    m_unfoldAllAct = make(tr("Unfold All"), QKeySequence(QStringLiteral("Ctrl+K, Ctrl+J")));
    m_searchAct = make(tr("Find in Files"), QKeySequence(C | S | K::Key_F), QStringLiteral(":/new-icons/search.svg"));

    connect(m_newWindowAct, &QAction::triggered, this, &MainWindow::newWindow);
    connect(m_newProjectAct, &QAction::triggered, this, &MainWindow::newProject);
    connect(m_openProjectAct, &QAction::triggered, this, &MainWindow::openProject);
    connect(m_openProjectFolderAct, &QAction::triggered, this, &MainWindow::openProject);
    connect(m_closeProjectAct, &QAction::triggered, this, &MainWindow::closeProject);
    connect(m_exitAct, &QAction::triggered, this, &QWidget::close);

    connect(m_newFileAct, &QAction::triggered, this, &MainWindow::newFile);
    connect(m_openFileAct, &QAction::triggered, this, &MainWindow::openFile);
    connect(m_saveAct, &QAction::triggered, m_editors, &EditorManager::saveCurrent);
    connect(m_saveAsAct, &QAction::triggered, m_editors, &EditorManager::saveCurrentAs);
    connect(m_saveAllAct, &QAction::triggered, m_editors, &EditorManager::saveAll);
    connect(m_closeFileAct, &QAction::triggered, m_editors, &EditorManager::closeCurrent);

    connect(m_undoAct, &QAction::triggered, this, [this] { forwardToFocus("undo"); });
    connect(m_redoAct, &QAction::triggered, this, [this] { forwardToFocus("redo"); });
    connect(m_cutAct, &QAction::triggered, this, [this] { forwardToFocus("cut"); });
    connect(m_copyAct, &QAction::triggered, this, [this] { forwardToFocus("copy"); });
    connect(m_pasteAct, &QAction::triggered, this, [this] { forwardToFocus("paste"); });
    connect(m_selectAllAct, &QAction::triggered, this, [this] { forwardToFocus("selectAll"); });
    connect(m_findAct, &QAction::triggered, m_editors, &EditorManager::showFind);
    connect(m_replaceAct, &QAction::triggered, m_editors, &EditorManager::showReplace);

    connect(m_projNewFileAct, &QAction::triggered, this, [this] { m_explorer->createFileIn(m_explorer->currentDirectory()); });
    connect(m_projNewFolderAct, &QAction::triggered, this, [this] { m_explorer->createFolderIn(m_explorer->currentDirectory()); });

    connect(m_explorerAct, &QAction::toggled, this, [this](bool on) {
        QWidget *island = m_side->parentWidget();
        if (!on) {
            if (island->isVisible())
                m_sideWidth = island->width(); // remembered so the panel comes back the same size
            island->hide();
            m_editors->focusEditor();
            return;
        }
        island->show();
        const int total = m_hsplit->width();
        const int w = qBound(150, m_sideWidth, qMax(150, total / 2));
        QList<int> sizes = m_hsplit->sizes();
        sizes[0] = w;
        sizes[1] = qMax(200, total - w - (sizes.size() > 2 ? sizes[2] : 0));
        m_hsplit->setSizes(sizes);
    });
    connect(m_terminalAct, &QAction::triggered, this, &MainWindow::toggleTerminal);
    connect(m_runAct, &QAction::triggered, this, &MainWindow::runCurrentFile);
    connect(m_runConfigAct, &QAction::triggered, this, &MainWindow::configureRun);
    connect(m_pythonPkgAct, &QAction::triggered, this, &MainWindow::showPythonPackages);
    connect(m_gradleAct, &QAction::triggered, this, &MainWindow::toggleGradlePanel);
    connect(m_fullscreenAct, &QAction::triggered, this, [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
    connect(m_wordWrapAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setWordWrap(on); });
    connect(m_breadcrumbsAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setShowBreadcrumbs(on); });
    m_explorer->setShowHidden(m_hiddenFilesAct->isChecked());
    connect(m_hiddenFilesAct, &QAction::toggled, this, [this](bool on) {
        SettingsManager::instance().setShowHiddenFiles(on);
        m_explorer->setShowHidden(on);
    });
    connect(m_minimapAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setShowMinimap(on); });
    connect(m_blameInlineAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setBlameInline(on); });
    connect(m_blameGutterAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setBlameGutter(on); });
    connect(m_stickyAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setStickyScroll(on); });
    connect(m_indentGuidesAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setIndentGuides(on); });
    connect(m_splitRightAct, &QAction::triggered, this, [this] { m_editors->splitCurrent(Qt::Horizontal); });
    connect(m_splitDownAct, &QAction::triggered, this, [this] { m_editors->splitCurrent(Qt::Vertical); });
    connect(m_nextTabAct, &QAction::triggered, m_editors, &EditorManager::nextTab);
    connect(m_prevTabAct, &QAction::triggered, m_editors, &EditorManager::previousTab);
    connect(m_aboutAct, &QAction::triggered, this, &MainWindow::about);
    connect(m_paletteAct, &QAction::triggered, this, &MainWindow::showCommandPalette);
    connect(m_quickOpenAct, &QAction::triggered, this, [this] { showQuickOpen(); });
    connect(m_gotoDefinitionAct, &QAction::triggered, this, [this] {
        if (CodeEditor *e = m_editors->currentEditor()) {
            const QTextCursor c = e->textCursor();
            goToDefinition(e, c.blockNumber(), c.positionInBlock());
        }
    });
    connect(m_renameAct, &QAction::triggered, this, &MainWindow::renameSymbol);
    connect(m_findRefsAct, &QAction::triggered, this, &MainWindow::findReferences);
    connect(m_gotoLineAct, &QAction::triggered, this, [this] { showQuickOpen(QStringLiteral(":")); });
    connect(m_bookmarkToggleAct, &QAction::triggered, this, [this] {
        if (CodeEditor *e = m_editors->currentEditor())
            e->toggleBookmark();
    });
    connect(m_bookmarkNextAct, &QAction::triggered, this, [this] { gotoBookmark(true); });
    connect(m_bookmarkPrevAct, &QAction::triggered, this, [this] { gotoBookmark(false); });
    connect(m_newTerminalAct, &QAction::triggered, this, [this] {
        showTerminal();
        m_terminal->newSession();
    });
    connect(m_previewAct, &QAction::triggered, this, &MainWindow::togglePreview);
    connect(m_showBookmarksAct, &QAction::triggered, this, [this] { showTasks(true); });
    connect(m_showTodosAct, &QAction::triggered, this, [this] { showTasks(false); });
    connect(m_gotoSymbolAct, &QAction::triggered, this, &MainWindow::showGoToSymbol);
    connect(m_searchAct, &QAction::triggered, this, &MainWindow::showSearch);
    auto onEditor = [this](void (CodeEditor::*fn)()) {
        return [this, fn] { if (auto *e = m_editors->currentEditor()) (e->*fn)(); };
    };
    connect(m_foldAct, &QAction::triggered, this, onEditor(&CodeEditor::foldCurrent));
    connect(m_unfoldAct, &QAction::triggered, this, onEditor(&CodeEditor::unfoldCurrent));
    connect(m_foldAllAct, &QAction::triggered, this, onEditor(&CodeEditor::foldAll));
    connect(m_unfoldAllAct, &QAction::triggered, this, onEditor(&CodeEditor::unfoldAll));
    connect(m_matchBracketAct, &QAction::triggered, this, [this] { if (auto *e = m_editors->currentEditor()) e->gotoMatchingBracket(); });
    createGitActions();

    // Window-wide shortcuts must also work while an editor (which handles Tab itself) has focus.
    for (QAction *a : {m_nextTabAct, m_prevTabAct})
        a->setShortcutContext(Qt::WindowShortcut);
    addAction(m_nextTabAct);
    addAction(m_prevTabAct);
}

void MainWindow::createMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_newWindowAct);
    file->addSeparator();
    file->addAction(m_newProjectAct);
    file->addAction(m_openProjectAct);
    m_recentMenu = file->addMenu(tr("Open Recent"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, [this] {
        m_recentMenu->clear();
        const QStringList recent = SettingsManager::instance().recentProjects();
        int n = 0;
        for (const QString &path : recent) {
            if (!QFileInfo(path).isDir())
                continue;
            m_recentMenu->addAction(QFileInfo(path).fileName() + QStringLiteral("  —  ") + FileManager::displayPath(path), this,
                                    [this, path] { openRecentProject(path); });
            ++n;
        }
        if (n == 0)
            m_recentMenu->addAction(tr("No recent projects"))->setEnabled(false);
        m_recentMenu->addSeparator();
        QAction *clear = m_recentMenu->addAction(tr("Clear Recent Projects"), this, [] { SettingsManager::instance().clearRecentProjects(); });
        clear->setEnabled(n > 0);
    });
    file->addAction(m_closeProjectAct);
    file->addSeparator();
    file->addAction(m_newFileAct);
    file->addAction(m_openFileAct);
    file->addAction(m_quickOpenAct);
    file->addSeparator();
    file->addAction(m_saveAct);
    file->addAction(m_saveAsAct);
    file->addAction(m_saveAllAct);
    QMenu *saveOptions = file->addMenu(tr("Save Options"));
    QMenu *autoSave = saveOptions->addMenu(tr("Auto Save"));
    autoSave->addAction(m_autoSaveOffAct);
    autoSave->addAction(m_autoSaveDelayAct);
    autoSave->addAction(m_autoSaveFocusAct);
    autoSave->addSeparator();
    autoSave->addAction(m_autoSaveDelayMsAct);
    saveOptions->addAction(m_trimAct);
    saveOptions->addAction(m_finalNewlineAct);
    saveOptions->addAction(m_formatOnSaveAct);
    file->addSeparator();
    file->addAction(m_closeFileAct);
    file->addSeparator();
    file->addAction(m_exitAct);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_undoAct);
    edit->addAction(m_redoAct);
    edit->addSeparator();
    edit->addAction(m_cutAct);
    edit->addAction(m_copyAct);
    edit->addAction(m_pasteAct);
    edit->addAction(m_selectAllAct);
    edit->addSeparator();
    edit->addAction(m_formatAct);
    edit->addSeparator();
    edit->addAction(m_findAct);
    edit->addAction(m_replaceAct);
    edit->addAction(m_gotoLineAct);
    edit->addAction(m_gotoSymbolAct);
    edit->addAction(m_gotoDefinitionAct);
    edit->addAction(m_findRefsAct);
    edit->addAction(m_renameAct);
    edit->addSeparator();
    edit->addAction(m_bookmarkToggleAct);
    edit->addAction(m_bookmarkNextAct);
    edit->addAction(m_bookmarkPrevAct);
    edit->addAction(m_matchBracketAct);
    edit->addAction(m_searchAct);
    edit->addSeparator();
    QMenu *folding = edit->addMenu(tr("Folding"));
    folding->addAction(m_foldAct);
    folding->addAction(m_unfoldAct);
    folding->addAction(m_foldAllAct);
    folding->addAction(m_unfoldAllAct);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_paletteAct);
    view->addSeparator();
    view->addAction(m_explorerAct);
    view->addAction(m_terminalAct);
    view->addAction(m_newTerminalAct);
    view->addAction(m_venvAct);
    view->addSeparator();
    view->addAction(m_runAct);
    view->addAction(m_runConfigAct);
    view->addAction(m_pythonPkgAct);
    view->addAction(m_gradleAct);
    view->addSeparator();
    view->addAction(m_splitRightAct);
    view->addAction(m_splitDownAct);
    view->addSeparator();
    view->addAction(m_wordWrapAct);
    view->addAction(m_previewAct);
    view->addAction(m_showBookmarksAct);
    view->addAction(m_showTodosAct);
    view->addSeparator();
    view->addAction(m_indentGuidesAct);
    view->addAction(m_stickyAct);
    view->addAction(m_minimapAct);
    view->addAction(m_hiddenFilesAct);
    view->addAction(m_breadcrumbsAct);
    QMenu *theme = view->addMenu(tr("Theme"));
    // Rebuilt on open: user themes come and go with the files in the themes folder.
    connect(theme, &QMenu::aboutToShow, this, [this, theme] {
        theme->clear();
        auto *group = new QActionGroup(theme);
        const QString active = SettingsManager::instance().theme();
        for (const ThemeManager::Info &info : ThemeManager::instance().themes()) {
            QAction *a = theme->addAction(info.name);
            a->setCheckable(true);
            a->setActionGroup(group);
            a->setChecked(info.id == active);
            const QString id = info.id;
            connect(a, &QAction::triggered, this, [id] { SettingsManager::instance().setTheme(id); });
        }
        theme->addSeparator();
        theme->addAction(tr("Edit Themes…"), this, &MainWindow::showThemeEditor);
        theme->addAction(tr("Open Themes Folder"), this, [] {
            QDir().mkpath(ThemeManager::instance().themesDir());
            QDesktopServices::openUrl(QUrl::fromLocalFile(ThemeManager::instance().themesDir()));
        });
    });
    view->addSeparator();
    view->addAction(m_fullscreenAct);

    QMenu *project = menuBar()->addMenu(tr("&Project"));
    project->addAction(m_projNewFileAct);
    project->addAction(m_projNewFolderAct);
    project->addSeparator();
    project->addAction(m_openProjectFolderAct);
    project->addAction(m_closeProjectAct);

    createGitMenu(menuBar()->addMenu(tr("&Git")));
    createLspMenu(menuBar()->addMenu(tr("&LSP")));

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_aboutAct);
}

void MainWindow::createToolBar()
{
    QToolBar *tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(16, 16));
    tb->addAction(m_explorerAct);
    tb->addSeparator();
    tb->addAction(m_openProjectAct);
    tb->addAction(m_closeProjectAct);
    tb->addAction(m_newFileAct);
    tb->addAction(m_projNewFolderAct);
    tb->addSeparator();
    tb->addAction(m_saveAct);
    tb->addAction(m_findAct);
    tb->addSeparator();
    tb->addAction(m_scmAct);
    tb->addAction(m_terminalAct);
    tb->addSeparator();
    m_mainToolBar = tb;
    // A web project runs its dev server instead of single files: the server controls replace the two run buttons.
    m_serverBar = new DevServerBar(this);
    m_serverBarAct = tb->addWidget(m_serverBar);
    connect(m_serverBar, &DevServerBar::availabilityChanged, this, [this] {
        updateRunToolbar();
        updateActions();
    });
    connect(m_serverBar, &DevServerBar::stateChanged, this, &MainWindow::updateActions);
    // An Expo app has its own run control (Android / iOS / Metro / prebuild / DevTools) and the app.json editor.
    m_expoSchema = new ExpoSchema(this);
    m_expoBar = new ExpoBar(this);
    m_expoBarAct = tb->addWidget(m_expoBar);
    connect(m_expoBar, &ExpoBar::availabilityChanged, this, &MainWindow::onExpoChanged);
    connect(m_expoBar, &ExpoBar::commandRequested, this, &MainWindow::runExpoCommand);
    connect(m_expoBar, &ExpoBar::devToolsRequested, this, [this] {
        if (!m_metroStarted) {
            statusBar()->showMessage(tr("Start Metro first (Run menu > Start Metro / Expo Go), then open DevTools"), 6000);
            return;
        }
        showTerminal();
        m_terminal->sendInput("j"); // the Expo CLI's key for the React Native DevTools
        statusBar()->showMessage(tr("Asked Expo to open React Native DevTools (the Metro session in the terminal must be running)"), 6000);
    });
    connect(m_expoBar, &ExpoBar::configureRequested, this, &MainWindow::showExpoConfig);
    connect(m_expoBar, &ExpoBar::schemaCacheRequested, this, &MainWindow::showExpoSchemaCache);
    connect(m_expoBar, &ExpoBar::appChanged, this, &MainWindow::pushExpoSchemas);
    connect(m_expoSchema, &ExpoSchema::ready, this, &MainWindow::pushExpoSchemas);
    connect(m_expoSchema, &ExpoSchema::failed, this, [this](const QString &, const QString &why) { statusBar()->showMessage(why, 8000); });
    connect(m_serverBar, &DevServerBar::openFileRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_serverBar, &DevServerBar::nestedProjectsFound, this, [this](const QStringList &dirs) {
        // After the project finished opening, so the question does not hold up restoring the session.
        QTimer::singleShot(0, this, [this, dirs] { askNestedWebProject(dirs); });
    });
    updateRunToolbar();
}

void MainWindow::askNestedWebProject(const QStringList &dirs)
{
    if (!m_projects->hasProject() || dirs.isEmpty())
        return;
    QString dir = dirs.first();
    if (dirs.size() > 1) {
        bool ok = false;
        dir = QInputDialog::getItem(this, tr("Web Projects Detected"),
                                    tr("Detected %1 web projects inside this project. Which one should QODE run?").arg(dirs.size()), dirs, 0,
                                    false, &ok);
        if (!ok)
            return;
    } else {
        QMessageBox box(QMessageBox::Question, tr("Web Project Detected"),
                        tr("Detected a web project at <b>%1</b>.<br>Do you want to run its dev server?").arg(dir.toHtmlEscaped()), {}, this);
        box.setInformativeText(tr("QODE will run it from that folder; the server controls then appear in the toolbar."));
        QPushButton *run = box.addButton(tr("Run Server"), QMessageBox::AcceptRole);
        box.addButton(tr("Not Now"), QMessageBox::RejectRole);
        QPushButton *never = box.addButton(tr("Don't Ask Again"), QMessageBox::DestructiveRole);
        box.setDefaultButton(run);
        box.exec();
        if (box.clickedButton() == never)
            m_serverBar->dontAskAboutNested();
        if (box.clickedButton() != run)
            return;
    }
    m_serverBar->useFolder(dir);
}

void MainWindow::updateRunToolbar()
{
    const bool web = m_serverBar->isAvailable();
    const bool expo = m_expoBar && m_expoBar->isAvailable();
    m_mainToolBar->removeAction(m_runAct);
    m_mainToolBar->removeAction(m_runConfigAct);
    m_mainToolBar->removeAction(m_pythonPkgAct);
    m_mainToolBar->removeAction(m_gradleAct);
    if (m_expoBarAct)
        m_expoBarAct->setVisible(expo);
    if (!web && !expo) {
        m_mainToolBar->insertAction(m_serverBarAct, m_runAct);
        m_mainToolBar->insertAction(m_serverBarAct, m_runConfigAct);
    }
    m_mainToolBar->insertAction(m_serverBarAct, m_gradleAct); // only in Gradle projects (updateGradleUi)
    m_mainToolBar->insertAction(m_serverBarAct, m_pythonPkgAct); // next to the run buttons; visible only with a virtual environment
    m_serverBarAct->setVisible(web);
    m_runAct->setText(web ? tr("Start / Restart Dev Server") : tr("Run File"));
    m_runAct->setToolTip(web ? tr("Start the dev server, or restart it (F5)") : tr("Run the project, or this file (F5)"));
    m_runConfigAct->setText(web ? tr("Dev Server Settings…") : tr("Run Configuration…"));
    m_runConfigAct->setToolTip(web ? tr("Package manager, script and port of the dev server")
                                   : tr("Set the command that runs the project or this type of file"));
}

// The Python Packages button sits next to Run and Run Configuration and exists only while the project has a virtual
// environment. A timer notices one created in the terminal (or deleted) a few seconds later.
void MainWindow::updatePythonUi()
{
    updateGradleUi();
    const QString root = m_projects->hasProject() ? m_projects->project().root : QString();
    const QString venv = root.isEmpty() ? QString() : PythonEnv::activeVenv(root);
    m_pythonPkgAct->setVisible(!venv.isEmpty());
    m_pythonPkgAct->setEnabled(m_projects->hasProject());
    if (!venv.isEmpty()) {
        const QString version = PythonEnv::venvVersion(root, venv);
        m_pythonPkgAct->setToolTip(tr("Python packages: install, upgrade and remove (%1%2)")
                                       .arg(venv, version.isEmpty() ? QString() : tr(", Python %1").arg(version)));
    }
    if (!m_venvTimer) {
        m_venvTimer = new QTimer(this);
        m_venvTimer->setInterval(3000);
        connect(m_venvTimer, &QTimer::timeout, this, &MainWindow::updatePythonUi);
    }
    if (root.isEmpty())
        m_venvTimer->stop();
    else if (!m_venvTimer->isActive())
        m_venvTimer->start();
    if (venv != m_activeVenv) {
        m_activeVenv = venv;
        restartPythonServer(); // the interpreter its imports resolve against changed
    }
}

// The language server reads the interpreter once at start: restart it after packages or the environment changed.
void MainWindow::restartPythonServer()
{
    for (const LspManager::ServerState &st : m_lsp->servers())
        if (st.id == QLatin1String("python") && (st.status == LspManager::Status::Running || st.status == LspManager::Status::Starting))
            m_lsp->restart(st.id);
}

// --- Expo -----------------------------------------------------------------------

void MainWindow::onExpoChanged()
{
    m_serverBar->setIgnoredDirs(m_expoBar->appDirs());
    updateRunToolbar();
    updateActions();
    if (!m_expoBar->isAvailable()) {
        m_lsp->setJsonSchemas({});
        return;
    }
    QSet<QString> asked;
    for (const ExpoApp &a : m_expoBar->apps())
        if (!a.appJson.isEmpty() && !asked.contains(a.sdk)) {
            asked.insert(a.sdk);
            m_expoSchema->request(a.sdk); // downloads once into ~/.cache/QODE/expo; ready() pushes it to the JSON server
        }
    pushExpoSchemas();
}

// The cached schema matching an app's SDK, else the newest cached one (empty when nothing is cached yet).
static QPair<QString, QJsonObject> expoSchemaFor(const QString &sdk)
{
    QString use = ExpoSchema::cachedSdks().contains(sdk) ? sdk : QString();
    if (use.isEmpty() && !ExpoSchema::cachedSdks().isEmpty())
        use = ExpoSchema::cachedSdks().first();
    return {use, use.isEmpty() ? QJsonObject() : ExpoSchema::load(use)};
}

// Tells the JSON language server which schema describes each app.json: completion, hover docs and validation.
void MainWindow::pushExpoSchemas()
{
    QMap<QString, QJsonArray> files; // schema sdk -> file URIs
    for (const ExpoApp &a : m_expoBar->apps())
        if (!a.appJson.isEmpty()) {
            const QString use = expoSchemaFor(a.sdk).first;
            if (!use.isEmpty())
                files[use].append(QUrl::fromLocalFile(a.appJson).toString());
        }
    QJsonArray schemas;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject schema = ExpoSchema::load(it.key());
        if (!schema.isEmpty())
            schemas.append(QJsonObject{{QStringLiteral("fileMatch"), it.value()}, {QStringLiteral("schema"), schema}});
    }
    m_lsp->setJsonSchemas(schemas);
}

void MainWindow::runExpoCommand(const QString &command, const QString &id)
{
    if (!m_editors->saveAll())
        return;
    if (id == QLatin1String("start") || id.startsWith(QLatin1String("android")) || id == QLatin1String("ios"))
        m_metroStarted = true; // run:android / run:ios end in the same interactive Metro session
    showTerminal();
    m_terminal->runCommand(command);
    m_terminal->focusTerminal();
}

void MainWindow::showExpoConfig()
{
    const ExpoApp *app = m_expoBar->currentApp();
    if (!app || app->appJson.isEmpty())
        return;
    if (m_expoDialog) {
        m_expoDialog->raise();
        m_expoDialog->activateWindow();
        return;
    }
    if (!m_editors->saveAll()) // the form edits the file on disk: unsaved text in an editor tab would be overwritten
        return;
    m_expoDialog = new ExpoConfigDialog(*app, expoSchemaFor(app->sdk).second, this);
    connect(m_expoDialog, &ExpoConfigDialog::openInEditorRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_expoDialog, &ExpoConfigDialog::saved, this, [this](const QString &path) {
        statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 5000);
        m_expoBar->setProjectRoot(m_projects->project().root); // the app name may have changed
    });
    m_expoDialog->show();
}

void MainWindow::showExpoSchemaCache()
{
    const QString dir = ExpoSchema::cacheDir();
    const QStringList sdks = ExpoSchema::cachedSdks();
    QMessageBox box(QMessageBox::NoIcon, tr("Expo Schema Cache"), QString(), {}, this);
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<b>Expo app.json schema</b>"));
    box.setInformativeText(
        tr("QODE downloads Expo's published schema (exp.host) once per SDK and caches it here. It powers completion, hover "
           "documentation and validation in app.json, and nothing is written into your projects.<br><br>"
           "<b>Cached:</b> %1<br><b>Folder:</b> %2 (%3)<br><br>"
           "<b>To remove it by hand:</b><br><code>rm -rf %2</code><br><br>"
           "It is downloaded again the next time you open an Expo project.")
            .arg(sdks.isEmpty() ? tr("nothing yet") : tr("SDK %1").arg(sdks.join(QStringLiteral(", "))).toHtmlEscaped(),
                 dir.toHtmlEscaped(), QLocale().formattedDataSize(ExpoSchema::cacheSize())));
    QPushButton *remove = box.addButton(tr("Delete Cache"), QMessageBox::DestructiveRole);
    QPushButton *copy = box.addButton(tr("Copy Command"), QMessageBox::ActionRole);
    box.addButton(tr("Close"), QMessageBox::RejectRole);
    remove->setEnabled(QFileInfo::exists(dir));
    box.exec();
    if (box.clickedButton() == copy) {
        QApplication::clipboard()->setText(QStringLiteral("rm -rf '%1'").arg(dir));
        statusBar()->showMessage(tr("Copied the removal command"), 4000);
    } else if (box.clickedButton() == remove) {
        if (ExpoSchema::clearCache()) {
            m_lsp->setJsonSchemas({});
            statusBar()->showMessage(tr("Deleted the Expo schema cache"), 5000);
        } else {
            QMessageBox::warning(this, tr("Expo Schema Cache"), tr("Could not delete %1").arg(dir));
        }
    }
}

void MainWindow::showPythonPackages()
{
    if (!m_projects->hasProject())
        return;
    if (m_pkgDialog) {
        m_pkgDialog->show();
        m_pkgDialog->raise();
        m_pkgDialog->activateWindow();
        return;
    }
    m_pkgDialog = new PackagesDialog(m_projects->project().root, this);
    connect(m_pkgDialog, &PackagesDialog::packagesChanged, this, &MainWindow::restartPythonServer);
    connect(m_pkgDialog, &PackagesDialog::environmentChanged, this, [this] {
        updatePythonUi();
        restartPythonServer();
    });
    connect(m_pkgDialog, &PackagesDialog::venvCreated, this, [this](const QString &name) {
        updatePythonUi();
        restartPythonServer();
        statusBar()->showMessage(tr("Created the virtual environment %1").arg(name), 6000);
        const QString shell = QFileInfo(qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash"))).fileName();
        const QString cmd = PythonEnv::activationCommand(m_projects->project().root, shell);
        if (cmd.isEmpty())
            return;
        if (QMessageBox::question(m_pkgDialog, tr("Activate it?"),
                                  tr("Activate %1 in the terminal now? New terminals activate it automatically.").arg(name)) == QMessageBox::Yes) {
            showTerminal();
            m_terminal->runCommand(cmd);
        }
    });
    m_pkgDialog->show();
}

void MainWindow::createStatusBar()
{
    auto mk = [this](int minWidth) {
        auto *l = new QLabel(this);
        l->setMinimumWidth(minWidth);
        statusBar()->addPermanentWidget(l);
        return l;
    };
    // Git indicator: branch switcher, ahead/behind counters and a changes pill, centred on one line.
    m_gitStatusWidget = new QWidget(this);
    auto *gl = new QHBoxLayout(m_gitStatusWidget);
    gl->setContentsMargins(2, 0, 2, 0);
    gl->setSpacing(4);
    m_branchButton = new BranchButton(true, m_gitStatusWidget);
    m_branchButton->setToolTip(tr("Switch or create branch"));
    connect(m_branchButton, &QAbstractButton::clicked, this, [this] { BranchPopup::open(m_git, m_branchButton, true); });
    auto counter = [this, gl](const QString &tip) {
        auto *b = new QToolButton(m_gitStatusWidget);
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(12, 12));
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 4px; font-size: 8pt; }"));
        b->hide();
        gl->addWidget(b, 0, Qt::AlignVCenter);
        return b;
    };
    gl->addWidget(m_branchButton, 0, Qt::AlignVCenter);
    m_behindBtn = counter(tr("Pull"));
    m_aheadBtn = counter(tr("Push"));
    connect(m_behindBtn, &QToolButton::clicked, m_git, &GitRepository::pull);
    connect(m_aheadBtn, &QToolButton::clicked, m_git, &GitRepository::push);
    m_changesPill = new QLabel(m_gitStatusWidget);
    m_changesPill->setObjectName(QStringLiteral("countPill"));
    m_changesPill->setFixedHeight(16);
    m_changesPill->setAlignment(Qt::AlignCenter);
    gl->addWidget(m_changesPill, 0, Qt::AlignVCenter);
    m_gitStatusWidget->hide();
    statusBar()->addWidget(m_gitStatusWidget);
    m_fileLabel = new QLabel(this);
    statusBar()->addWidget(m_fileLabel, 1);
    m_lspButton = new QToolButton(this);
    m_lspButton->setAutoRaise(true);
    m_lspButton->setPopupMode(QToolButton::InstantPopup);
    m_lspButton->setCursor(Qt::PointingHandCursor);
    m_lspButton->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 6px; } QToolButton::menu-indicator { image: none; }"));
    m_lspButton->setMenu(m_lspMenu); // the same menu as LSP in the menu bar (created first)
    statusBar()->addPermanentWidget(m_lspButton);
    m_langLabel = mk(90);
    m_encLabel = mk(70);
    m_eolLabel = mk(40);
    m_posLabel = mk(110);
    m_modeLabel = mk(36);
    statusBar()->setSizeGripEnabled(false);
    // A temporary message is painted over the left side of the bar, so nothing there may stay visible under it.
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString &msg) {
        const bool idle = msg.isEmpty();
        m_fileLabel->setVisible(idle);
        m_gitStatusWidget->setVisible(idle && m_git->isRepo());
    });
}

void MainWindow::restoreSettings()
{
    auto &s = SettingsManager::instance();
    resize(1200, 760);
    if (!s.windowGeometry().isEmpty())
        restoreGeometry(s.windowGeometry());
    m_sideWidth = s.explorerWidth();
    m_hsplit->setSizes({m_sideWidth, qMax(400, width() - m_sideWidth)});
    if (!s.sideBarVisible())
        m_explorerAct->setChecked(false);
    m_terminalHeight = s.terminalHeight();
}

// --- Startup / session ------------------------------------------------------

void MainWindow::openInitialPaths(const QStringList &paths, bool restoreLastSession)
{
    if (paths.isEmpty()) {
        if (restoreLastSession)
            restoreSession();
        return;
    }
    bool projectOpened = false;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir() && !projectOpened) {
            openProjectPath(fi.absoluteFilePath());
            projectOpened = true;
        } else if (fi.isFile()) {
            m_editors->openFile(fi.absoluteFilePath());
        } else if (!fi.exists()) {
            QMessageBox::warning(this, tr("Unable to open"), tr("\"%1\" does not exist.").arg(p));
        }
    }
}

void MainWindow::saveSession()
{
    auto &s = SettingsManager::instance();
    s.setLastProject(m_projects->hasProject() ? m_projects->root() : QString());
    s.setOpenFiles(m_editors->openFilePaths());
    s.setActiveFile(m_editors->currentDocument() ? m_editors->currentDocument()->filePath() : QString());
    if (m_projects->hasProject())
        s.setProjectLayout(currentLayout());
}

// What "layout" means for a project: panel visibility and sizes plus how the editors are split.
QJsonObject MainWindow::currentLayout() const
{
    const bool sideShown = m_explorerAct->isChecked();
    const int sideWidth = sideShown && m_side->isVisible() ? m_hsplit->sizes().value(0) : m_sideWidth;
    const bool termShown = m_terminal->isVisible();
    const int termHeight = termShown ? m_vsplit->sizes().value(1, m_terminalHeight) : m_terminalHeight;
    QJsonArray sections;
    for (bool b : m_side->expandedStates())
        sections.append(b);
    QJsonObject o;
    o.insert(QStringLiteral("sideVisible"), sideShown);
    o.insert(QStringLiteral("sideWidth"), sideWidth);
    o.insert(QStringLiteral("sideSections"), sections);
    o.insert(QStringLiteral("terminalVisible"), termShown);
    o.insert(QStringLiteral("terminalHeight"), termHeight);
    o.insert(QStringLiteral("editors"), m_editors->layoutState());
    return o;
}

void MainWindow::applyLayout(const QJsonObject &o)
{
    if (o.isEmpty())
        return; // a project without a saved layout keeps whatever the window looks like
    m_sideWidth = o.value(QStringLiteral("sideWidth")).toInt(m_sideWidth);
    m_terminalHeight = o.value(QStringLiteral("terminalHeight")).toInt(m_terminalHeight);
    QList<bool> sections;
    for (const QJsonValue &v : o.value(QStringLiteral("sideSections")).toArray())
        sections << v.toBool();
    if (!sections.isEmpty())
        m_side->setExpandedStates(sections);

    const bool sideShown = o.value(QStringLiteral("sideVisible")).toBool(true);
    if (m_explorerAct->isChecked() != sideShown) {
        const int w = m_sideWidth;
        m_explorerAct->setChecked(sideShown);
        m_sideWidth = w; // hiding records the current width, which is not the one to keep
    } else if (sideShown) {
        QList<int> sizes = m_hsplit->sizes();
        const int total = sizes.value(0) + sizes.value(1);
        sizes[0] = qBound(150, m_sideWidth, qMax(150, m_hsplit->width() / 2));
        sizes[1] = qMax(200, total - sizes[0]);
        m_hsplit->setSizes(sizes);
    }

    const bool termShown = o.value(QStringLiteral("terminalVisible")).toBool(false);
    if (termShown && !m_terminal->isVisible()) {
        showTerminal();
        m_terminal->ensureStarted();
    } else if (!termShown && m_terminal->isVisible()) {
        toggleTerminal();
    } else if (termShown) {
        const int total = m_vsplit->height();
        const int h = qBound(80, m_terminalHeight, qMax(80, total - 100));
        m_vsplit->setSizes({total - h, h});
    }
}

void MainWindow::restoreSession()
{
    auto &s = SettingsManager::instance();
    const QString project = s.lastProject();
    // A project another window already has open is skipped quietly, so a second QODE starts empty.
    if (!project.isEmpty() && QFileInfo(project).isDir() && !InstanceRegistry::instance().isHeldElsewhere(InstanceRegistry::Project, project)) {
        QString err;
        m_projects->openProject(project, &err); // silently skip if it can't be opened
    }
    // The project's open files are reopened by onProjectOpened.
}

// --- Language servers ------------------------------------------------------------------------------------

void MainWindow::createLspMenu(QMenu *menu)
{
    m_lspMenu = menu;
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::rebuildLspMenu);
}

namespace {

// A non-clickable line inside a menu (headers, status text), styled with the theme.
void addMenuLabel(QMenu *menu, const QString &html)
{
    auto *label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setStyleSheet(QStringLiteral("background: transparent; padding: 4px 14px;"));
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    menu->addAction(action);
}

// "SET UP", "REMOVE", ...: a small spaced-out caption that starts a group of items.
void addMenuSection(QMenu *menu, const QString &title, const Theme &theme, bool separator = true)
{
    if (separator)
        menu->addSeparator();
    auto *label = new QLabel(title.toUpper());
    QFont f = label->font();
    f.setBold(true);
    f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    label->setFont(f);
    label->setStyleSheet(QStringLiteral("background: transparent; color: %1; padding: 6px 14px 2px 14px;").arg(theme.textMuted.name()));
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    menu->addAction(action);
}

QIcon dotIcon(const QColor &color)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(4, 4, 8, 8));
    return QIcon(pm);
}

} // namespace

void MainWindow::rebuildLspMenu()
{
    // Submenus are children of the menu and are not removed by clear().
    const QList<QMenu *> old = m_lspMenu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(old);
    m_lspMenu->clear();
    m_lspMenu->setToolTipsVisible(true);
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    using Status = LspManager::Status;

    addMenuSection(m_lspMenu, tr("Language servers"), theme, false);
    for (const LspManager::ServerState &st : m_lsp->servers()) {
        const QString id = st.id;
        QString state;
        QColor color = theme.textMuted;
        switch (st.status) {
        case Status::Running: state = tr("Running"); color = theme.gitAdded; break;
        case Status::Starting: state = tr("Starting…"); color = theme.gitModified; break;
        case Status::NotFound: state = tr("Not installed"); break;
        case Status::Crashed: state = tr("Stopped"); color = theme.gitConflict; break;
        default: state = st.path.isEmpty() ? tr("Idle") : tr("Ready"); color = theme.accent; break;
        }
        QMenu *sub = m_lspMenu->addMenu(dotIcon(color), QStringLiteral("%1    %2").arg(st.name, state));
        sub->setToolTipsVisible(true);

        // Status block
        QString head = QStringLiteral("<span style='font-size:11pt'><b>%1</b></span><br><span style='color:%2'>&#9679; %3</span>")
                           .arg(st.name.toHtmlEscaped(), color.name(), state);
        const QString where = !st.detail.isEmpty() && st.status != Status::Running ? st.detail : st.path;
        if (!where.isEmpty())
            head += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(theme.textMuted.name(), where.toHtmlEscaped());
        if (!st.progress.isEmpty())
            head += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(theme.textMuted.name(), st.progress.toHtmlEscaped());
        addMenuLabel(sub, head);

        const LspServerSpec *spec = LspServers::byId(id);
        const bool managed = spec && LspServers::isManaged(*spec, st.path);
        const bool installed = st.status != Status::NotFound;

        addMenuSection(sub, tr("Set up"), theme);
        if (st.installable && !installed)
            connect(sub->addAction(tr("Download and Set Up…")), &QAction::triggered, this, [this, id] { installLspServer(id); });
        else if (managed)
            connect(sub->addAction(tr("Update / Reinstall…")), &QAction::triggered, this, [this, id] { installLspServer(id); });
        if (!installed)
            connect(sub->addAction(tr("How to Install…")), &QAction::triggered, this, [this, id] { showLspInstallHelp(id); });
        connect(sub->addAction(tr("Set Server Path…")), &QAction::triggered, this, [this, id] { chooseLspServerPath(id); });
        if (!m_lsp->serverPath(id).isEmpty())
            connect(sub->addAction(tr("Auto-detect Server on PATH")), &QAction::triggered, this, [this, id] { m_lsp->setServerPath(id, {}); });

        if (installed) {
            addMenuSection(sub, tr("Server"), theme);
            if (id == QLatin1String("java") || id == QLatin1String("kotlin")) {
                QAction *sync = sub->addAction(tr("Sync Build Files (Gradle / Maven)"));
                sync->setEnabled(st.status == Status::Running && m_lsp->hasBuildFiles());
                sync->setToolTip(tr("Make the server re-read build.gradle(.kts), settings.gradle, libs.versions.toml and pom.xml"));
                connect(sync, &QAction::triggered, this, &MainWindow::syncBuildFiles);
            }
            connect(sub->addAction(tr("Restart Server")), &QAction::triggered, this, [this, id] { m_lsp->restart(id); });
            if (st.status == Status::Running)
                connect(sub->addAction(tr("Show Server Log…")), &QAction::triggered, this, [this, id] { showLspLog(id); });
        }

        addMenuSection(sub, tr("Remove"), theme);
        QAction *rm = sub->addAction(tr("Remove Language Server…"));
        rm->setEnabled(installed);
        rm->setToolTip(installed ? tr("Shows what is deleted or which commands to run") : tr("Not installed"));
        connect(rm, &QAction::triggered, this, [this, id] { removeLspServer(id); });
    }

    addMenuSection(m_lspMenu, tr("Project"), theme);
    {
        const bool has = m_lsp->hasBuildFiles();
        const bool syncing = [&] {
            for (const LspManager::ServerState &st : m_lsp->servers())
                if (st.syncing)
                    return true;
            return false;
        }();
        QAction *sync = m_lspMenu->addAction(syncing ? tr("⟳  Syncing Build Files…") : tr("⟳  Sync Build Files (Gradle / Maven)"));
        sync->setEnabled(has && !syncing);
        sync->setToolTip(!has ? tr("No Gradle or Maven build file in this project")
                         : !m_lsp->canSyncBuildFiles() ? tr("Open a Java or Kotlin file first so its language server starts")
                                                       : tr("Re-import the project after editing build.gradle(.kts), settings.gradle, libs.versions.toml or pom.xml"));
        connect(sync, &QAction::triggered, this, &MainWindow::syncBuildFiles);
    }
    QString detectedFrom;
    m_lsp->detectedFlags(&detectedFrom);
    QAction *flags = m_lspMenu->addAction(m_lsp->hasCompileDatabase() ? tr("Compiler Flags…")
                                          : !detectedFrom.isEmpty()   ? tr("Compiler Flags… (detected from %1)").arg(detectedFrom)
                                                                      : tr("Compiler Flags… (no compile_commands.json)"));
    flags->setEnabled(m_projects->hasProject());
    flags->setToolTip(m_projects->hasProject() ? tr("Flags used for C/C++ when the project has no compile_commands.json")
                                               : tr("Open a project first"));
    connect(flags, &QAction::triggered, this, &MainWindow::editCompilerFlags);
    QAction *includes = m_lspMenu->addAction(tr("Include Paths…"));
    includes->setEnabled(m_projects->hasProject());
    includes->setToolTip(m_projects->hasProject() ? tr("Extra folders searched for #include in this project's C/C++ files")
                                                  : tr("Open a project first"));
    connect(includes, &QAction::triggered, this, &MainWindow::editIncludePaths);

    addMenuSection(m_lspMenu, tr("Diagnostics"), theme);
    addMenuLabel(m_lspMenu, tr("<span style='color:%1'>%2 errors, %3 warnings in open files</span>")
                                .arg(theme.textMuted.name())
                                .arg(m_lsp->diagnosticCount(LspDiagnostic::Error))
                                .arg(m_lsp->diagnosticCount(LspDiagnostic::Warning)));
}

// LSP > Sync Build Files: the Java / Kotlin servers re-import the Gradle / Maven project.
void MainWindow::syncBuildFiles()
{
    if (!m_lsp->hasBuildFiles()) {
        statusBar()->showMessage(tr("No Gradle or Maven build file found in this project"), 5000);
        return;
    }
    if (!m_editors->saveAll())
        return;
    if (!m_lsp->syncBuildFiles(true)) {
        statusBar()->showMessage(tr("Open a Java or Kotlin file first — the language server starts with it and reads the build files itself"), 7000);
        return;
    }
    statusBar()->showMessage(tr("Syncing build files…"), 5000);
}

void MainWindow::updateLspStatus()
{
    using Status = LspManager::Status;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QString text = tr("LSP");
    QColor color = t.textMuted;
    QString tip = tr("Language servers");
    for (const LspManager::ServerState &st : m_lsp->servers()) {
        if (st.documents == 0 || st.companion)
            continue; // nothing open that this server handles (companions report through the diagnostics counts)
        const QString name = st.name.section(QLatin1Char(' '), 0, 0);
        switch (st.status) {
        case Status::NotFound:
            text = tr("%1 not found").arg(name);
            color = t.gitModified;
            tip = tr("%1 is not installed. Click for instructions.").arg(name);
            if (!m_lspHintShown) {
                m_lspHintShown = true;
                statusBar()->showMessage(st.installable ? tr("%1 not found — choose LSP > Download and Set Up to install it").arg(name)
                                                        : tr("%1 not found — open the LSP menu to see how to install it").arg(name),
                                         8000);
            }
            break;
        case Status::Starting:
            text = tr("%1 starting…").arg(name);
            if (!st.progress.isEmpty())
                tip = st.progress;
            break;
        case Status::Crashed:
            text = tr("%1 stopped").arg(name);
            color = t.gitConflict;
            tip = st.detail;
            break;
        case Status::Running: {
            text = name;
            const int e = m_lsp->diagnosticCount(LspDiagnostic::Error), w = m_lsp->diagnosticCount(LspDiagnostic::Warning);
            if (e)
                text += QStringLiteral("  ✕ %1").arg(e);
            if (w)
                text += QStringLiteral("  ⚠ %1").arg(w);
            color = e ? t.gitConflict : (w ? t.gitModified : t.gitAdded);
            tip = tr("%1 — running").arg(st.detail.isEmpty() ? name : st.detail);
            if (st.syncing) {
                text += tr("  ⟳ Syncing build files…");
                tip += tr("\nSyncing the Gradle / Maven project — completion and errors may be incomplete until it ends");
                if (!st.progress.isEmpty())
                    tip += QStringLiteral(" (%1)").arg(st.progress);
            } else if (!st.progress.isEmpty()) { // e.g. a Gradle import: the server is up but still working
                text += QStringLiteral("  ⟳ ") + (st.progress.size() > 40 ? st.progress.left(39) + QStringLiteral("…") : st.progress);
                tip += QLatin1Char('\n') + st.progress;
            }
            break;
        }
        default:
            break;
        }
        break;
    }
    m_lspButton->setText(text);
    m_lspButton->setToolTip(tip);
    m_lspButton->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 6px; color: %1; } QToolButton::menu-indicator { image: none; }")
                                   .arg(color.name()));
}

// Jumps to where the symbol at (line, column) of `editor` is defined; several candidates are offered in a menu.
void MainWindow::goToDefinition(CodeEditor *editor, int line, int column)
{
    Document *doc = nullptr;
    for (Document *d : m_editors->documents())
        if (m_editors->editorFor(d) == editor)
            doc = d;
    if (!doc || !m_lsp->isServed(doc)) {
        statusBar()->showMessage(tr("Go to Definition needs a running language server for this file"), 4000);
        return;
    }
    // The name under the caret: when a library only offers compiled classes, the source file is searched for it.
    QTextCursor at(editor->document()->findBlockByNumber(line));
    at.setPosition(at.position() + column);
    at.select(QTextCursor::WordUnderCursor);
    const QString word = at.selectedText();
    QPointer<CodeEditor> guard(editor);
    m_lsp->definition(doc, line, column, [this, guard, word, line, column](const QVector<LspLocation> &locations) {
        if (locations.isEmpty()) {
            const QString why = JarSource::lastFailure();
            if (why.isEmpty()) {
                statusBar()->showMessage(tr("No definition found"), 3000);
                return;
            }
            statusBar()->showMessage(why, 6000);
            const JarSource::Coordinates c = JarSource::coordinatesOf(JarSource::lastMissingSourcesJar());
            if (guard && c.isValid() && !m_projects->project().root.isEmpty() &&
                QMessageBox::question(this, tr("Library sources"),
                                      tr("%1 has no sources jar on this computer.\n\nDownload %2 sources with Gradle? Nothing is added to your project.").arg(why, c.text())) ==
                    QMessageBox::Yes)
                fetchLibrarySources(c, guard, line, column);
            return;
        }
        auto open = [this, word](const LspLocation &loc) {
            int target = loc.line;
            int col = loc.column;
            if (loc.fromClass) {
                // Only the class was known: jump to where the name under the caret is declared, else the top.
                target = 0;
                col = 0;
                QFile f(loc.path);
                if (!word.isEmpty() && f.open(QIODevice::ReadOnly)) {
                    const QRegularExpression re(QStringLiteral("\\b(?:class|object|interface|fun|val|var|typealias|enum class|data class|sealed class)\\s+(?:<[^>]*>\\s*)?(?:[\\w.<>?, ]+\\.)?(") +
                                                QRegularExpression::escape(word) + QStringLiteral(")\\b"));
                    int n = 0;
                    while (!f.atEnd()) {
                        const QString text = QString::fromUtf8(f.readLine());
                        const QRegularExpressionMatch m = re.match(text);
                        if (m.hasMatch()) {
                            target = n;
                            col = int(m.capturedStart(1));
                            break;
                        }
                        ++n;
                    }
                }
            }
            m_editors->openFileAt(loc.path, target + 1, col + 1);
        };
        if (locations.size() == 1 || !guard) {
            open(locations.first());
            return;
        }
        QMenu menu;
        const QString root = m_projects->project().root;
        for (const LspLocation &loc : locations) {
            const QString shown = root.isEmpty() ? loc.path : QDir(root).relativeFilePath(loc.path);
            connect(menu.addAction(QStringLiteral("%1:%2").arg(shown).arg(loc.line + 1)), &QAction::triggered, this,
                    [open, loc] { open(loc); });
        }
        menu.exec(QCursor::pos());
    });
}

// Downloads <coords>:sources with the project's own Gradle (its repositories), through an init script kept in QODE's
// cache so the project stays untouched, then repeats the Go to Definition that needed it.
void MainWindow::fetchLibrarySources(const JarSource::Coordinates &coords, CodeEditor *editor, int line, int column)
{
    const QString root = m_projects->project().root;
    const QString wrapper = root + QStringLiteral("/gradlew");
    const QString program = QFileInfo(wrapper).isExecutable() ? wrapper : QStandardPaths::findExecutable(QStringLiteral("gradle"));
    if (program.isEmpty()) {
        statusBar()->showMessage(tr("Gradle was not found (no gradlew in the project and no gradle on PATH)"), 6000);
        return;
    }
    const QString dir = Platform::runDir();
    QDir().mkpath(dir);
    const QString script = dir + QStringLiteral("/library-sources.init.gradle");
    QFile f(script);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("// Written by QODE: downloads the sources jar of one library into Gradle's cache.\n"
                "rootProject { p ->\n"
                "    p.tasks.register('qodeLibrarySources') {\n"
                "        doLast {\n"
                "            def dep = p.dependencies.create(p.property('qode.sources') + ':sources@jar')\n"
                "            dep.transitive = false\n"
                "            p.configurations.detachedConfiguration(dep).resolve()\n"
                "        }\n"
                "    }\n"
                "}\n");
        f.close();
    }
    statusBar()->showMessage(tr("Downloading sources of %1…").arg(coords.text()));
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(root);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QPointer<CodeEditor> guard(editor);
    connect(proc, &QProcess::finished, this, [this, proc, coords, guard, line, column](int code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(proc->readAll());
        proc->deleteLater();
        if (code != 0) {
            const QStringList lines = out.trimmed().split(QLatin1Char('\n'));
            statusBar()->showMessage(tr("Could not download the sources of %1 — %2").arg(coords.text(), lines.isEmpty() ? QString() : lines.last().trimmed()), 10000);
            return;
        }
        statusBar()->showMessage(tr("Sources of %1 downloaded").arg(coords.text()), 4000);
        if (guard)
            goToDefinition(guard, line, column);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            statusBar()->showMessage(tr("Could not start Gradle"), 6000);
            proc->deleteLater();
        }
    });
    proc->start(program, {QStringLiteral("-q"), QStringLiteral("-I"), script, QStringLiteral("-Pqode.sources=") + coords.text(), QStringLiteral("qodeLibrarySources")});
}

// Hover link on a gray import: let the user choose which unused imports go.
void MainWindow::removeUnusedImports(CodeEditor *editor)
{
    const QVector<int> lines = editor->unusedImportLines();
    if (lines.isEmpty())
        return;
    QStringList texts;
    for (const int n : lines)
        texts << editor->document()->findBlockByNumber(n).text();
    QString name = tr("this file");
    for (Document *d : m_editors->documents())
        if (m_editors->editorFor(d) == editor)
            name = d->fileName();
    UnusedImportsDialog dlg(name, lines, texts, this);
    if (dlg.exec() == QDialog::Accepted)
        editor->removeLines(dlg.selectedLines());
}

// Alt+Enter: the server's quick fixes ("Import → java.io.File") and refactorings for the caret / selection.
void MainWindow::showCodeActions(Document *doc, CodeEditor *editor, int startLine, int startColumn, int endLine, int endColumn)
{
    if (!m_lsp->isServed(doc)) {
        statusBar()->showMessage(tr("Quick fixes need a running language server for this file"), 4000);
        return;
    }
    QPointer<CodeEditor> guard(editor);
    m_lsp->codeActions(doc, startLine, startColumn, endLine, endColumn, [this, doc, guard, startLine, startColumn, endLine, endColumn](const QVector<LspCodeAction> &found) {
        if (!guard)
            return;
        QVector<LspCodeAction> actions = found;
        // An import that no installed package satisfies: offer to install it into the project's virtual environment.
        const QStringList missing = unresolvedPythonImports(doc, startLine, startColumn, endLine, endColumn);
        auto rank = [](const LspCodeAction &a) { return a.kind.startsWith(QLatin1String("quickfix")) ? 0 : a.kind.startsWith(QLatin1String("source")) ? 1 : 2; };
        std::stable_sort(actions.begin(), actions.end(), [&](const LspCodeAction &a, const LspCodeAction &b) { return rank(a) < rank(b); });
        if (actions.isEmpty() && missing.isEmpty()) {
            statusBar()->showMessage(tr("No quick fixes available here"), 3000);
            return;
        }
        auto *menu = new QMenu(guard);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        // Rounded and bordered like the tooltips: a translucent window lets the stylesheet's radius show.
        menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        menu->setAttribute(Qt::WA_TranslucentBackground);
        const Theme theme = Theme::byName(SettingsManager::instance().theme());
        menu->setStyleSheet(QStringLiteral("QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 4px; }"
                                           "QMenu::item { padding: 5px 22px 5px 12px; border-radius: 5px; }"
                                           "QMenu::item:selected { background: %4; }")
                                .arg(theme.panel.name(), theme.editorFg.name(), theme.border.name(), theme.selection.name()));
        const bool hasVenv = m_projects->hasProject() && !PythonEnv::activeVenv(m_projects->project().root).isEmpty();
        for (const QString &package : missing) {
            QString title = hasVenv ? tr("Install \"%1\" in the virtual environment…").arg(package)
                                    : tr("Install \"%1\" (set up a virtual environment first)…").arg(package);
            title.replace(QLatin1Char('&'), QStringLiteral("&&"));
            connect(menu->addAction(title), &QAction::triggered, this, [this, package, hasVenv] {
                showPythonPackages();
                if (m_pkgDialog && hasVenv)
                    m_pkgDialog->installNow({package});
            });
        }
        for (const LspCodeAction &a : std::as_const(actions)) {
            QString title = a.title;
            title.replace(QLatin1Char('&'), QStringLiteral("&&"));
            connect(menu->addAction(title), &QAction::triggered, this, [this, doc, a] {
                m_lsp->runCodeAction(doc, a, [this](bool ok) {
                    if (!ok)
                        statusBar()->showMessage(tr("The language server could not apply that action"), 4000);
                });
            });
        }
        menu->popup(guard->viewport()->mapToGlobal(guard->cursorRect().bottomLeft() + QPoint(0, 2)));
    });
}

// The packages that would fix an "import could not be resolved" diagnostic touching the range (Python only): the module's
// top-level name mapped to its PyPI package, skipping relative imports and modules that are the project's own.
QStringList MainWindow::unresolvedPythonImports(Document *doc, int startLine, int startColumn, int endLine, int endColumn) const
{
    QStringList out;
    if (!m_projects->hasProject())
        return out;
    static const QRegularExpression re(QStringLiteral("^Import \"([^\"]+)\" could not be resolved"));
    const QString root = m_projects->project().root;
    for (const LspDiagnostic &d : m_lsp->diagnostics(doc->filePath())) {
        if (d.server != QLatin1String("python") || (d.code != QLatin1String("reportMissingImports") && d.code != QLatin1String("reportMissingModuleSource")))
            continue;
        const bool before = d.endLine < startLine || (d.endLine == startLine && d.endColumn < startColumn);
        const bool after = d.startLine > endLine || (d.startLine == endLine && d.startColumn > endColumn);
        const auto m = re.match(d.message);
        if (before || after || !m.hasMatch() || m.captured(1).startsWith(QLatin1Char('.')))
            continue;
        const QString top = m.captured(1).section(QLatin1Char('.'), 0, 0);
        const QDir dir(root);
        if (dir.exists(top) || dir.exists(top + QStringLiteral(".py")) || dir.exists(QStringLiteral("src/") + top))
            continue; // the project's own module: a path problem, not a missing package
        const QString package = PythonTools::packageForModule(m.captured(1));
        if (!out.contains(package))
            out << package;
    }
    return out;
}

// A WorkspaceEdit from the server (import added, rename, ...): open files are edited in place as one undo step each.
bool MainWindow::applyWorkspaceEdit(const QJsonObject &edit)
{
    bool ok = false;
    const QHash<QString, QVector<LspTextEdit>> byPath = LspManager::editsOf(edit, &ok);
    if (!ok || byPath.isEmpty())
        return false;
    for (auto it = byPath.constBegin(); it != byPath.constEnd(); ++it) {
        Document *doc = m_editors->documentForPath(it.key());
        if (!doc && m_editors->openFile(it.key()))
            doc = m_editors->documentForPath(it.key());
        CodeEditor *ed = doc ? m_editors->editorFor(doc) : nullptr;
        if (!ed || !ed->applyTextEdits(it.value()))
            return false;
    }
    return true;
}

void MainWindow::editCompilerFlags()
{
    QString source;
    const QStringList detected = m_lsp->detectedFlags(&source);
    CompilerFlagsDialog dlg(m_lsp->projectFlagsText(), source, detected, this);
    if (dlg.exec() == QDialog::Accepted)
        m_lsp->setProjectFlagsText(dlg.text());
}

void MainWindow::editIncludePaths()
{
    const QString root = m_projects->project().root;
    QString note;
    if (QFileInfo::exists(root + QStringLiteral("/compile_flags.txt")) && LspManager::compileDatabasePath(root).isEmpty())
        note = tr("This project has a <b>compile_flags.txt</b>, which the language server uses instead of these paths. "
                  "Add the <b>-I</b> lines there.");
    else if (!LspManager::compileDatabasePath(root).isEmpty())
        note = tr("This project has a <b>compile_commands.json</b>; the paths are added to every entry of a copy kept in "
                  "QODE's cache. Your project files are not changed, and your build does not use these paths.");
    IncludePathsDialog dlg(root, SettingsManager::instance().includePaths(), note, this);
    if (dlg.exec() == QDialog::Accepted)
        m_lsp->setIncludePaths(dlg.paths());
}

void MainWindow::showLspInstallHelp(const QString &serverId)
{
    QMessageBox box(QMessageBox::Information, tr("Language server not found"), m_lsp->installHelp(serverId), QMessageBox::Ok, this);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
}

void MainWindow::installLspServer(const QString &serverId)
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    if (!spec)
        return;
    QString what;
    if (spec->installer == LspServerSpec::Installer::Npm) {
        NpmInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        what = tr("Web language servers");
    } else if (spec->installer == LspServerSpec::Installer::Pip) {
        PipInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        what = tr("Python language tools");
    } else if (spec->installer == LspServerSpec::Installer::Jdtls) {
        JdtlsInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        what = tr("Java language server");
    } else {
        LspInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        what = tr("Kotlin language server");
    }
    // A path chosen earlier would shadow the new install; clearing it also restarts the server. One install can
    // provide several servers (the web ones), so all of them are refreshed.
    for (const LspServerSpec *s : LspServers::sharingInstall(*spec))
        m_lsp->setServerPath(s->id, {});
    statusBar()->showMessage(tr("%1 installed").arg(what), 5000);
}

void MainWindow::removeLspServer(const QString &serverId)
{
    auto *dlg = new LspRemoveDialog(serverId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal); // the terminal must stay usable for the sudo password
    connect(dlg, &LspRemoveDialog::removalStarting, m_lsp, &LspManager::shutdown);
    connect(dlg, &LspRemoveDialog::terminalCommandRequested, this, [this](const QString &cmd) {
        showTerminal();
        m_terminal->runCommand(cmd);
    });
    connect(dlg, &LspRemoveDialog::restartRequested, this, [this] { QTimer::singleShot(0, this, &MainWindow::restartApplication); });
    dlg->show();
}

// Closes this window (saving the session like a normal quit) and starts QODE again once the project claim is released.
void MainWindow::restartApplication()
{
    if (!close())
        return;
    QProcess::startDetached(QStringLiteral("/bin/sh"),
                            {QStringLiteral("-c"), QStringLiteral("sleep 1; exec \"$0\""), QCoreApplication::applicationFilePath()});
}

void MainWindow::chooseLspServerPath(const QString &serverId)
{
    const QString current = m_lsp->serverPath(serverId);
    const QString path = QFileDialog::getOpenFileName(this, tr("Select the language server executable"),
                                                      current.isEmpty() ? QStringLiteral("/usr/bin") : current);
    if (!path.isEmpty())
        m_lsp->setServerPath(serverId, path);
}

void MainWindow::showLspLog(const QString &serverId)
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    auto *dlg = new LspLogDialog(spec ? spec->displayName : serverId, [this, serverId] { return m_lsp->logOf(serverId); }, this);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

// --- Projects -----------------------------------------------------------------

// Every window is its own process: no shared state, and one crashing or closing never affects another.
void MainWindow::newWindow()
{
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--new-window")});
}

namespace {
// Run settings key of the command that runs the whole project (every other key is a file type).
const QString kProjectRunKey = QStringLiteral("@project");
} // namespace

void MainWindow::newProject()
{
    NewProjectDialog dlg(SettingsManager::instance().lastDirectory(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (m_projects->hasProject() && !closeProject())
        return;
    QString err;
    SettingsManager::instance().setLastDirectory(dlg.location());

    const ProjectTemplates::Template *tpl = ProjectTemplates::find(dlg.templateId());
    if (!tpl) {
        if (!m_projects->createProject(dlg.projectName(), dlg.location(), &err))
            QMessageBox::warning(this, tr("Unable to create project"), err);
        return;
    }

    const QString dest = QDir(dlg.location()).filePath(dlg.projectName());
    const ProjectTemplates::Result made = ProjectTemplates::instantiate(*tpl, dlg.values(), dest);
    if (!made.ok) {
        QMessageBox::warning(this, tr("Unable to create project"), made.error);
        return;
    }
    if (dlg.initGit()) {
        QProcess git;
        git.setWorkingDirectory(dest);
        git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
        if (!git.waitForFinished(5000) || git.exitCode() != 0)
            QMessageBox::warning(this, tr("Git"), tr("The project was created, but \"git init\" failed."));
    }
    if (!m_projects->openProject(dest, &err)) {
        QMessageBox::warning(this, tr("Unable to open project"), err);
        return;
    }
    if (!made.runCommand.isEmpty())
        SettingsManager::instance().setRunCommand(kProjectRunKey, made.runCommand);
    for (const QString &file : made.openFiles)
        m_editors->openFile(file);
}

void MainWindow::openProject()
{
    const QString start = m_projects->hasProject() ? QFileInfo(m_projects->root()).absolutePath()
                                                   : SettingsManager::instance().lastDirectory();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Project"), start.isEmpty() ? QDir::homePath() : start);
    if (!dir.isEmpty())
        openProjectPath(dir);
}

void MainWindow::openProjectPath(const QString &path)
{
    if (m_projects->hasProject() && QFileInfo(path).canonicalFilePath() == m_projects->root())
        return; // already open here
    if (InstanceRegistry::instance().isHeldElsewhere(InstanceRegistry::Project, path)) {
        QMessageBox::information(this, tr("Project already open"),
                                 tr("\"%1\" is currently open in another QODE window.\n\nClose it there first, or switch to that window.")
                                     .arg(FileManager::displayPath(path)));
        return;
    }
    if (m_projects->hasProject() && !closeProject())
        return;
    QString err;
    if (!m_projects->openProject(path, &err))
        QMessageBox::warning(this, tr("Unable to open project"), tr("Unable to open project.\n\nPath:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
    else {
        SettingsManager::instance().setLastDirectory(QFileInfo(path).absolutePath());
        // The project's saved layout may have the sidebar hidden; opening a project yourself with no file tree and the
        // welcome page still showing looks like nothing happened.
        if (!m_explorerAct->isChecked())
            m_explorerAct->setChecked(true);
    }
}

bool MainWindow::closeProject()
{
    if (!m_projects->hasProject())
        return true;
    saveSession(); // remember this project's open files before they are closed
    if (!m_editors->closeAll())
        return false;
    m_projects->closeProject();
    return true;
}

void MainWindow::refreshRecentProjects()
{
    m_editors->setRecentProjects(SettingsManager::instance().recentProjects());
}

void MainWindow::openRecentProject(const QString &path)
{
    if (!QFileInfo(path).isDir()) {
        QMessageBox::warning(this, tr("Unable to open project"), tr("\"%1\" no longer exists.").arg(FileManager::displayPath(path)));
        SettingsManager::instance().removeRecentProject(path);
        return;
    }
    openProjectPath(path);
}

void MainWindow::onProjectOpened(const Project &p)
{
    InstanceRegistry::instance().setClaims(InstanceRegistry::Project, {p.root});
    SettingsManager::instance().setProject(p.root);
    m_lsp->setProjectRoot(p.root);
    m_expoBar->setProjectRoot(p.root); // before the web-server bar: it must not offer `expo start` as a web server
    m_serverBar->setIgnoredDirs(m_expoBar->appDirs());
    m_serverBar->setProjectRoot(p.root);
    onExpoChanged();
    m_bookmarks = SettingsManager::instance().bookmarks();
    m_tasks->setBookmarks(m_bookmarks);
    SettingsManager::instance().addRecentProject(p.root);
    m_explorer->setProjectRoot(p.root);
    m_mover->setProjectRoot(p.root);
    m_projectFiles->setRoot(p.root);
    m_editors->setProjectRoot(p.root);
    m_searchPanel->setProjectRoot(p.root);
    m_tasks->setProjectRoot(p.root);
    m_media->setProjectRoot(p.root);
    m_git->setWorkDirectory(p.root);
    // The shell always starts in the project root.
    m_terminal->setWorkingDirectory(p.root);
    m_terminal->setSavedTabs(SettingsManager::instance().terminalTabs());
    if (m_terminal->isRunning())
        m_terminal->restartAll();
    else if (m_terminal->isVisible())
        m_terminal->ensureStarted();
    updateTitle();
    updateActions();
    updatePythonUi();

    // Reopen what was open in this project last time.
    auto &s = SettingsManager::instance();
    for (const QString &f : s.openFiles())
        if (QFileInfo(f).isFile())
            m_editors->openFile(f);
    m_editors->restoreLayout(s.projectLayout().value(QStringLiteral("editors")).toObject());
    applyLayout(s.projectLayout());
    const QString active = s.activeFile();
    if (!active.isEmpty() && QFileInfo(active).isFile())
        m_editors->openFile(active);
}

void MainWindow::onProjectClosed()
{
    InstanceRegistry::instance().setClaims(InstanceRegistry::Project, {});
    SettingsManager::instance().setProject({});
    m_lsp->setProjectRoot({});
    m_expoBar->setProjectRoot({});
    m_serverBar->setIgnoredDirs({});
    m_serverBar->setProjectRoot({});
    m_metroStarted = false;
    onExpoChanged();
    m_bookmarks.clear();
    m_tasks->setBookmarks(m_bookmarks);
    m_explorer->setProjectRoot({});
    m_mover->setProjectRoot({});
    m_projectFiles->setRoot({});
    m_editors->setProjectRoot({});
    m_searchPanel->setProjectRoot({});
    m_tasks->setProjectRoot({});
    hideMedia();
    m_media->setProjectRoot({});
    m_git->setWorkDirectory({});
    m_terminal->setWorkingDirectory(QDir::homePath());
    updatePythonUi();
    m_terminal->stop();
    updateTitle();
    updateActions();
}

// --- Files --------------------------------------------------------------------

void MainWindow::newFile()
{
    if (m_projects->hasProject())
        m_explorer->createFileIn(m_explorer->currentDirectory());
    else
        m_editors->newUntitled();
}

void MainWindow::openFile()
{
    const QString start = m_projects->hasProject() ? m_projects->root() : SettingsManager::instance().lastDirectory();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open File"), start.isEmpty() ? QDir::homePath() : start);
    for (const QString &f : files)
        m_editors->openFile(f);
}

void MainWindow::forwardToFocus(const char *slot)
{
    if (QWidget *w = QApplication::focusWidget())
        QMetaObject::invokeMethod(w, slot);
}

// --- UI state -----------------------------------------------------------------

void MainWindow::updateActions()
{
    const bool hasProject = m_projects->hasProject();
    const bool hasDoc = m_editors->currentDocument() != nullptr;
    m_closeProjectAct->setEnabled(hasProject);
    m_projNewFileAct->setEnabled(hasProject);
    m_projNewFolderAct->setEnabled(hasProject);
    m_saveAct->setEnabled(hasDoc);
    m_saveAsAct->setEnabled(hasDoc);
    m_saveAllAct->setEnabled(!m_editors->modifiedDocuments().isEmpty());
    m_closeFileAct->setEnabled(hasDoc);
    m_findAct->setEnabled(hasDoc);
    m_matchBracketAct->setEnabled(hasDoc);
    m_formatAct->setEnabled(hasDoc);
    for (QAction *a : {m_foldAct, m_unfoldAct, m_foldAllAct, m_unfoldAllAct})
        a->setEnabled(hasDoc);
    m_replaceAct->setEnabled(hasDoc);
    if ((m_serverBar && m_serverBar->isAvailable()) || (m_expoBar && m_expoBar->isAvailable())) {
        m_runAct->setEnabled(true);
        m_runConfigAct->setEnabled(true);
    } else {
        const bool project = m_projects->hasProject();
        m_runAct->setEnabled(project || !currentFilePath().isEmpty() || (hasDoc && m_editors->currentDocument()->isUntitled()));
        m_runConfigAct->setEnabled(project || !currentFilePath().isEmpty());
    }

    const bool repo = m_git->isRepo();
    const QString path = currentFilePath();
    const bool fileInRepo = repo && !path.isEmpty() && !m_git->relativePath(path).isEmpty();
    const GitFileChange *change = fileInRepo ? m_git->changeFor(path) : nullptr;
    m_gitRefreshAct->setEnabled(hasProject);
    m_gitFetchAct->setEnabled(repo);
    m_gitPullAct->setEnabled(repo);
    m_gitPushAct->setEnabled(repo);
    m_gitNewBranchAct->setEnabled(repo);
    m_gitInitAct->setEnabled(hasProject && !repo && m_git->gitAvailable() && m_git->isResolved());
    m_gitStageFileAct->setEnabled(change && (change->isUnstaged() || change->untracked || change->conflicted));
    m_gitUnstageFileAct->setEnabled(change && change->isStaged());
    m_gitDiscardFileAct->setEnabled(change && (change->isUnstaged() || change->untracked));
    m_gitDiffFileAct->setEnabled(fileInRepo);
    m_nextChangeAct->setEnabled(m_editors->currentEditor() && m_editors->currentEditor()->hasDiffBase());
    m_prevChangeAct->setEnabled(m_nextChangeAct->isEnabled());
}

void MainWindow::updateTitle()
{
    Document *d = m_editors->currentDocument();
    QStringList parts;
    if (d)
        parts << d->fileName() + (d->isModified() ? QStringLiteral(" *") : QString());
    if (m_projects->hasProject())
        parts << m_projects->project().name;
    parts << QStringLiteral("QODE");
    setWindowTitle(parts.join(QStringLiteral(" — ")));
}

void MainWindow::updateStatus()
{
    Document *d = m_editors->currentDocument();
    CodeEditor *e = m_editors->currentEditor();
    if (!d || !e) {
        for (QLabel *l : {m_fileLabel, m_langLabel, m_encLabel, m_eolLabel, m_posLabel, m_modeLabel})
            l->clear();
        return;
    }
    m_fileLabel->setText(d->isUntitled() ? d->fileName() : FileManager::displayPath(d->filePath()));
    m_langLabel->setText(d->languageName());
    m_encLabel->setText(d->encoding());
    m_eolLabel->setText(d->lineEndingName());
    m_posLabel->setText(tr("Ln %1, Col %2").arg(e->currentLine()).arg(e->currentColumn()));
    m_modeLabel->setText(e->overwriteMode() ? tr("OVR") : tr("INS"));
}

// The Gradle button exists only in Gradle projects (Java, Kotlin or Compose Desktop alike).
void MainWindow::updateGradleUi()
{
    const QString dir = m_projects->hasProject() ? GradleTasks::buildDir(m_projects->project().root) : QString();
    m_gradleAct->setVisible(!dir.isEmpty());
    m_gradle->setBuildDir(dir);
    if (dir.isEmpty() && m_rightStack->currentWidget() == m_gradle && m_rightStack->parentWidget()->isVisible())
        hideMedia();
}

void MainWindow::toggleGradlePanel()
{
    if (m_rightStack->parentWidget()->isVisible() && m_rightStack->currentWidget() == m_gradle) {
        hideMedia();
        return;
    }
    disconnect(m_previewConn);
    m_previewDoc = nullptr;
    m_previewTimer->stop();
    m_media->clear();
    m_md->clear();
    m_rightStack->setCurrentWidget(m_gradle);
    openRightPanel();
    m_gradle->ensureLoaded();
}

void MainWindow::openRightPanel()
{
    QWidget *island = m_rightStack->parentWidget();
    if (island->isVisible())
        return;
    island->show();
    QList<int> sizes = m_hsplit->sizes();
    const int w = qBound(280, m_mediaWidth, qMax(280, m_hsplit->width() / 2));
    sizes[2] = w;
    sizes[1] = qMax(200, sizes[1] - w);
    m_hsplit->setSizes(sizes);
}

void MainWindow::showMedia(const QString &path)
{
    disconnect(m_previewConn);
    m_previewDoc = nullptr;
    m_md->clear();
    m_media->openMedia(path);
    m_rightStack->setCurrentWidget(m_media);
    openRightPanel();
}

void MainWindow::hideMedia()
{
    QWidget *island = m_rightStack->parentWidget();
    if (island->isVisible())
        m_mediaWidth = island->width();
    disconnect(m_previewConn);
    m_previewDoc = nullptr;
    m_previewTimer->stop();
    m_media->clear();
    m_md->clear();
    island->hide();
}

// The Preview button / shortcut: shows (or closes) the live preview of the current Markdown or SVG file.
void MainWindow::togglePreview()
{
    Document *doc = m_editors->currentDocument();
    if (!EditorManager::isPreviewable(doc)) {
        statusBar()->showMessage(tr("Preview is available for Markdown and SVG files"), 3000);
        return;
    }
    if (m_rightStack->parentWidget()->isVisible() && m_previewDoc == doc) {
        hideMedia();
        return;
    }
    m_media->clear();
    m_md->clear();
    m_previewDoc = doc;
    disconnect(m_previewConn);
    m_previewConn = connect(doc->textDocument(), &QTextDocument::contentsChanged, m_previewTimer, qOverload<>(&QTimer::start));
    openRightPanel();
    updatePreview();
}

void MainWindow::updatePreview()
{
    if (!m_previewDoc) {
        return;
    }
    const QString path = m_previewDoc->filePath();
    if (m_previewDoc->languageName() == QLatin1String("Markdown")) {
        m_rightStack->setCurrentWidget(m_md);
        m_md->setMarkdown(path, m_previewDoc->text());
    } else {
        m_rightStack->setCurrentWidget(m_media);
        m_media->previewSvg(path, m_previewDoc->text());
    }
}

// An open preview follows the active tab when that is another Markdown / SVG file.
void MainWindow::followPreview()
{
    if (!m_previewDoc || !m_rightStack->parentWidget()->isVisible())
        return;
    Document *doc = m_editors->currentDocument();
    if (doc == m_previewDoc || !EditorManager::isPreviewable(doc))
        return;
    m_previewDoc = doc;
    disconnect(m_previewConn);
    m_previewConn = connect(doc->textDocument(), &QTextDocument::contentsChanged, m_previewTimer, qOverload<>(&QTimer::start));
    m_media->clear();
    m_md->clear();
    updatePreview();
}

void MainWindow::showTerminal()
{
    if (m_terminal->isVisible())
        return;
    m_terminal->parentWidget()->show();
    const int total = m_vsplit->height();
    const int h = qBound(80, m_terminalHeight, qMax(80, total - 100));
    m_vsplit->setSizes({total - h, h});
    m_terminalAct->setChecked(true);
}

// The command a recognised project type runs with (Gradle application, qmake application), or an empty string.
static QString detectedRunCommand(const QString &root, QString *note, QList<QPair<QString, QString>> *configs = nullptr)
{
    if (root.isEmpty())
        return {};
    const JvmProject jvm = JvmProject::detect(root);
    if (jvm.isValid()) {
        if (note)
            *note = MainWindow::tr("<b>%1</b> project detected: <b>%2</b> is the suggested run command. Pick another detected configuration below if it fits better.")
                        .arg(jvm.kind().toHtmlEscaped(), jvm.runCommand().toHtmlEscaped());
        if (configs)
            for (const JvmProject::Config &c : jvm.configs())
                configs->append({c.name, c.command});
        return jvm.runCommand();
    }
    const QmakeProject qmake = QmakeProject::detect(root);
    if (qmake.isValid() && qmake.isApp()) {
        if (note)
            *note = MainWindow::tr("Qt project <b>%1</b> detected: this command builds it with qmake in <b>build/</b> and runs the result.")
                        .arg(qmake.proFileName().toHtmlEscaped());
        return qmake.runCommand();
    }
    return {};
}

void MainWindow::configureRun()
{
    if (m_serverBar->isAvailable()) {
        m_serverBar->configure();
        return;
    }
    if (!m_projects->hasProject() && currentFilePath().isEmpty())
        return;
    const QString path = currentFilePath();
    const QString root = m_projects->project().root;
    const QString key = RunConfigDialog::keyFor(path);
    SettingsManager &s = SettingsManager::instance();
    const QString projectCmd = s.runCommand(kProjectRunKey);
    const QString fileCmd = path.isEmpty() ? QString() : s.runCommand(key);
    QString note;
    QList<QPair<QString, QString>> configs;
    const QString detected = detectedRunCommand(root, projectCmd.isEmpty() ? &note : nullptr, &configs);
    // A recognised project runs as a whole; a file type keeps its own command until the user picks project-wide.
    const bool wide = !projectCmd.isEmpty() || (fileCmd.isEmpty() && !detected.isEmpty());
    const QString command = !projectCmd.isEmpty() ? projectCmd : !fileCmd.isEmpty() ? fileCmd : detected;
    if (fileCmd.isEmpty() && projectCmd.isEmpty() && !detected.isEmpty())
        note += tr(" Change it if you run the project differently.");
    else
        note.clear();

    RunConfigDialog dlg(path, command, this);
    dlg.setNote(note);
    dlg.setProjectRoot(root);
    dlg.setSuggestions(configs);
    dlg.setProjectWide(wide);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (dlg.projectWide()) {
        s.setRunCommand(kProjectRunKey, dlg.command());
    } else {
        s.setRunCommand(kProjectRunKey, QString());
        if (!path.isEmpty())
            s.setRunCommand(key, dlg.command());
    }
}

void MainWindow::showThemeEditor()
{
    if (!m_themeEditor)
        m_themeEditor = new ThemeEditorDialog(this);
    m_themeEditor->setAttribute(Qt::WA_DeleteOnClose);
    m_themeEditor->show();
    m_themeEditor->raise();
    m_themeEditor->activateWindow();
}

void MainWindow::runCurrentFile()
{
    // In an Expo app F5 repeats the last toolbar configuration (Metro by default), unless this file type has its own command.
    if (m_expoBar->isAvailable()) {
        const QString file = currentFilePath();
        const bool ownCommand = !file.isEmpty() && !SettingsManager::instance().runCommand(RunConfigDialog::keyFor(file)).isEmpty();
        if (!ownCommand && (m_expoBar->appForFile(file) || !m_serverBar->isAvailable())) {
            m_expoBar->runLast();
            return;
        }
    }
    runFile();
}

void MainWindow::runFile()
{
    // In a web project F5 means the dev server, unless this file type has a command of its own.
    if (m_serverBar->isAvailable()) {
        const QString file = currentFilePath();
        if (file.isEmpty() || SettingsManager::instance().runCommand(RunConfigDialog::keyFor(file)).isEmpty()) {
            if (!m_editors->saveAll())
                return;
            m_serverBar->start(); // restarts when it is already running
            return;
        }
    }
    SettingsManager &s = SettingsManager::instance();
    const QString root = m_projects->project().root;
    QString path = currentFilePath();
    Document *doc = m_editors->currentDocument();
    if (doc && doc->isUntitled() && !m_editors->saveCurrent())
        return;
    path = currentFilePath();

    // No command yet for this file or project: ask for one (prefilled for recognised project types).
    if (s.runCommand(kProjectRunKey).isEmpty() && (path.isEmpty() || s.runCommand(RunConfigDialog::keyFor(path)).isEmpty())) {
        if (path.isEmpty() && detectedRunCommand(root, nullptr).isEmpty())
            return;
        configureRun();
        if (s.runCommand(kProjectRunKey).isEmpty() && (path.isEmpty() || s.runCommand(RunConfigDialog::keyFor(path)).isEmpty()))
            return;
    }

    const QString projectCmd = s.runCommand(kProjectRunKey);
    if (!projectCmd.isEmpty()) {
        // The project command builds/runs everything, so every open file has to be on disk.
        if (!m_editors->saveAll())
            return;
        showTerminal();
        m_terminal->runCommand(RunConfigDialog::expand(projectCmd, path, root));
        m_terminal->focusTerminal();
        return;
    }
    if (doc && doc->isModified() && !m_editors->saveCurrent())
        return;
    showTerminal();
    m_terminal->runCommand(RunConfigDialog::expand(s.runCommand(RunConfigDialog::keyFor(path)), path, root));
    m_terminal->focusTerminal();
}

void MainWindow::toggleTerminal()
{
    const bool show = !m_terminal->isVisible();
    if (show) {
        showTerminal();
        m_terminal->ensureStarted();
        m_terminal->focusTerminal();
    } else {
        // Remember the height for the rest of the session.
        m_terminalHeight = m_vsplit->sizes().value(1, m_terminalHeight);
        m_terminal->parentWidget()->hide();
        m_editors->focusEditor();
    }
    m_terminalAct->setChecked(show);
}

namespace {

QString plainText(QString t)
{
    t.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    t.remove(QLatin1Char('&'));
    t.replace(QLatin1Char('\x01'), QLatin1Char('&'));
    return t;
}

// Flattens a menu tree into palette entries ("Git › Switch Branch: main").
void collectActions(QMenu *menu, const QString &category, const QSet<QAction *> &exclude, QSet<QAction *> &seen,
                    QList<PalettePopup::Item> &out)
{
    emit menu->aboutToShow(); // lazily filled menus (branches) populate themselves here
    for (QAction *a : menu->actions()) {
        if (a->isSeparator())
            continue;
        if (QMenu *sub = a->menu()) {
            const QString name = plainText(a->text());
            collectActions(sub, category.isEmpty() ? name : category + QStringLiteral(" › ") + name, exclude, seen, out);
            continue;
        }
        if (exclude.contains(a) || seen.contains(a) || !a->isEnabled() || !a->isVisible())
            continue;
        seen.insert(a);
        PalettePopup::Item it;
        it.title = plainText(a->text());
        it.detail = category;
        it.hint = a->shortcut().toString(QKeySequence::NativeText);
        if (a->isCheckable() && a->isChecked())
            it.hint = it.hint.isEmpty() ? QStringLiteral("✓") : QStringLiteral("✓  ") + it.hint;
        it.icon = a->icon();
        it.data = QVariantList{QVariant::fromValue(a), category + QLatin1Char('|') + it.title};
        out.append(it);
    }
}

} // namespace

void MainWindow::showCommandPalette()
{
    QList<PalettePopup::Item> items;
    QSet<QAction *> seen;
    for (QAction *top : menuBar()->actions())
        if (QMenu *m = top->menu())
            collectActions(m, plainText(top->text()), {m_paletteAct}, seen, items);

    // Recently used commands come first while the query is empty.
    const QStringList recent = SettingsManager::instance().recentCommands();
    auto rank = [&recent](const PalettePopup::Item &it) { return recent.indexOf(it.data.toList().value(1).toString()); };
    std::stable_sort(items.begin(), items.end(), [&](const auto &a, const auto &b) {
        const int ra = rank(a), rb = rank(b);
        if ((ra < 0) != (rb < 0))
            return ra >= 0;
        return ra >= 0 && ra < rb;
    });

    auto *pop = new PalettePopup(this);
    pop->setPlaceholder(tr("Type a command…"));
    pop->setEmptyText(tr("No matching commands"));
    pop->setItems(items);
    connect(pop, &PalettePopup::accepted, this, [this](const QVariant &data, const QString &) {
        const QVariantList parts = data.toList();
        QPointer<QAction> action = parts.value(0).value<QAction *>();
        if (!action)
            return;
        auto &settings = SettingsManager::instance();
        QStringList recent = settings.recentCommands();
        recent.removeAll(parts.value(1).toString());
        recent.prepend(parts.value(1).toString());
        settings.setRecentCommands(recent.mid(0, 12));
        // Let the popup hand focus back first so the command acts on the right widget.
        QTimer::singleShot(0, this, [action] {
            if (action && action->isEnabled())
                action->trigger();
        });
    });
    pop->popup();
}

void MainWindow::noteRecentFile()
{
    const QString path = currentFilePath();
    if (path.isEmpty())
        return;
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > 40)
        m_recentFiles.removeLast();
}

void MainWindow::showTasks(bool bookmarks)
{
    if (!m_explorerAct->isChecked())
        m_explorerAct->setChecked(true);
    m_side->setExpanded(3, true);
    if (bookmarks)
        m_tasks->showBookmarks();
    else
        m_tasks->showTodos();
}

QString MainWindow::lineTextOf(const QString &path, int line) const
{
    for (Document *d : m_editors->documents())
        if (d->filePath() == path)
            return d->textDocument()->findBlockByNumber(line).text();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    for (int i = 0; !f.atEnd(); ++i) {
        const QByteArray l = f.readLine();
        if (i == line)
            return QString::fromUtf8(l).trimmed();
    }
    return {};
}

// Single point of change for bookmarks: updates the store, the open editor, the panel and the settings.
void MainWindow::applyBookmarks(const QString &path, const QList<int> &lines)
{
    if (lines.isEmpty())
        m_bookmarks.remove(path);
    else
        m_bookmarks.insert(path, lines);
    for (Document *d : m_editors->documents())
        if (d->filePath() == path)
            if (CodeEditor *e = m_editors->editorFor(d); e && e->bookmarks() != lines)
                e->setBookmarks(lines);
    SettingsManager::instance().setBookmarks(m_bookmarks);
    m_tasks->setBookmarks(m_bookmarks);
}

// Next / previous bookmark across all files, wrapping around.
void MainWindow::gotoBookmark(bool next)
{
    QList<QPair<QString, int>> all;
    QStringList paths = m_bookmarks.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString &p : paths)
        for (int l : m_bookmarks.value(p))
            all.append({p, l});
    if (all.isEmpty()) {
        statusBar()->showMessage(tr("No bookmarks — press Ctrl+F2 to add one"), 3000);
        return;
    }
    QString curPath;
    int curLine = -1;
    if (Document *d = m_editors->currentDocument(); d && m_editors->currentEditor()) {
        curPath = d->filePath();
        curLine = m_editors->currentEditor()->textCursor().blockNumber();
    }
    const QPair<QString, int> cur{curPath, curLine};
    QPair<QString, int> target = next ? all.first() : all.last(); // wrap-around default
    if (next) {
        for (const auto &b : std::as_const(all))
            if (b > cur) {
                target = b;
                break;
            }
    } else {
        for (auto it = all.crbegin(); it != all.crend(); ++it)
            if (*it < cur) {
                target = *it;
                break;
            }
    }
    m_editors->openFileAt(target.first, target.second + 1, 1);
}

void MainWindow::showSearch()
{
    if (!m_explorerAct->isChecked())
        m_explorerAct->setChecked(true);
    m_side->setExpanded(2, true);
    QString prefill;
    if (CodeEditor *e = m_editors->currentEditor()) {
        const QString sel = e->textCursor().selectedText();
        if (!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator) && sel.size() < 200)
            prefill = sel;
    }
    m_searchPanel->focusQuery(prefill);
}

void MainWindow::replaceInFiles(const QStringList &paths, const SearchOptions &options, const QString &replacement)
{
    const QRegularExpression rx = options.toRegex();
    if (!rx.isValid())
        return;
    int total = 0, changedFiles = 0;
    QStringList failed;
    for (const QString &path : paths) {
        int n = 0;
        if (Document *doc = m_editors->documentForPath(path)) {
            n = ProjectSearch::replaceInDocument(doc->textDocument(), rx, replacement);
        } else {
            QString text;
            bool bom = false;
            if (!ProjectSearch::readTextFile(path, &text, &bom)) {
                failed << path;
                continue;
            }
            n = ProjectSearch::replaceInText(&text, rx, replacement);
            if (n > 0) {
                QSaveFile f(path);
                QByteArray data = text.toUtf8();
                if (bom)
                    data.prepend("\xEF\xBB\xBF");
                if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
                    failed << path;
                    continue;
                }
            }
        }
        if (n > 0) {
            total += n;
            ++changedFiles;
        }
    }
    statusBar()->showMessage(tr("Replaced %1 in %2").arg(total == 1 ? tr("1 occurrence") : tr("%1 occurrences").arg(total),
                                                          changedFiles == 1 ? tr("1 file") : tr("%1 files").arg(changedFiles)),
                             8000);
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Replace in Files"),
                             tr("These files could not be changed (binary, not UTF-8, or not writable):\n\n%1").arg(failed.join(QLatin1Char('\n'))));
    m_searchPanel->rerun();
}

void MainWindow::showQuickOpen(const QString &initialQuery)
{
    m_projectFiles->ensureFresh();
    const QString root = m_projectFiles->root();
    const QDir rootDir(root);

    // Builds the entry list from the index, listing recently active files first.
    auto build = [this, root, rootDir]() {
        auto makeItem = [&](const QString &path) {
            PalettePopup::Item it;
            const QFileInfo fi(path);
            it.title = fi.fileName();
            const QString dir = fi.absolutePath();
            if (!root.isEmpty() && (dir == root || dir.startsWith(root + QLatin1Char('/'))))
                it.detail = dir == root ? QString() : rootDir.relativeFilePath(dir);
            else
                it.detail = FileManager::displayPath(dir);
            it.icon = FileIcons::forFile(it.title);
            it.data = path;
            return it;
        };
        QList<PalettePopup::Item> items;
        QSet<QString> used;
        for (const QString &p : std::as_const(m_recentFiles))
            if (QFileInfo::exists(p) && !used.contains(p)) {
                used.insert(p);
                items.append(makeItem(p));
            }
        for (const QString &p : m_projectFiles->files())
            if (!used.contains(p))
                items.append(makeItem(p));
        return items;
    };

    auto *pop = new PalettePopup(this);
    pop->setMode(PalettePopup::Mode::Paths);
    pop->setLineSuffixEnabled(true);
    pop->setPlaceholder(tr("Search files by name — append :line to jump to a line"));
    pop->setEmptyText(m_projects->hasProject() ? tr("No matching files") : tr("Open a project to search its files"));
    pop->setItems(build());
    auto updateStatusLine = [this, pop] { pop->setStatus(m_projectFiles->isScanning() ? tr("Indexing project files…") : QString()); };
    updateStatusLine();
    connect(m_projectFiles, &ProjectFiles::updated, pop, [pop, build, updateStatusLine] {
        pop->setItems(build());
        updateStatusLine();
    });
    connect(pop, &PalettePopup::accepted, this, [this](const QVariant &data, const QString &query) {
        static const QRegularExpression suffix(QStringLiteral(":(\\d+)(?::(\\d+))?$"));
        const auto m = suffix.match(query.trimmed());
        const int line = m.hasMatch() ? m.captured(1).toInt() : 0;
        const int column = m.hasMatch() ? m.captured(2).toInt() : 0;
        const QString path = data.toString();
        QTimer::singleShot(0, this, [this, path, line, column] {
            if (path.isEmpty())
                m_editors->gotoLine(line, column); // ":42" on its own goes to a line of the current file
            else if (line > 0)
                m_editors->openFileAt(path, line, column);
            else
                m_editors->openFile(path);
        });
    });
    pop->popup();
    if (!initialQuery.isEmpty())
        pop->setQuery(initialQuery);
}

void MainWindow::showGoToSymbol()
{
    Document *doc = m_editors->currentDocument();
    if (!doc)
        return;
    QList<PalettePopup::Item> items;
    for (const Breadcrumbs::Symbol &sym : Breadcrumbs::documentSymbols(doc->textDocument(), doc->languageName())) {
        PalettePopup::Item it;
        it.title = sym.name;
        it.detail = sym.parent;
        it.hint = QString::number(sym.line + 1);
        it.data = sym.line + 1;
        items.append(it);
    }
    auto *pop = new PalettePopup(this);
    pop->setPlaceholder(tr("Go to symbol in %1").arg(doc->fileName()));
    pop->setEmptyText(tr("No symbols found"));
    pop->setItems(items);
    connect(pop, &PalettePopup::accepted, this, [this](const QVariant &data, const QString &) {
        const int line = data.toInt();
        QTimer::singleShot(0, this, [this, line] { m_editors->gotoLine(line); });
    });
    pop->popup();
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About QODE"),
                       tr("<h3>QODE %1</h3><p>A native, lightweight C++/Qt code editor built around "
                          "projects, files, code and an integrated terminal.</p>")
                           .arg(QCoreApplication::applicationVersion()));
}

// --- Window events --------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_editors->confirmDiscardOrSaveAll()) {
        event->ignore();
        return;
    }
    saveSession();
    auto &s = SettingsManager::instance();
    s.setWindowGeometry(saveGeometry());
    if (m_side->isVisible())
        m_sideWidth = m_hsplit->sizes().value(0);
    s.setExplorerWidth(m_sideWidth);
    s.setSideBarVisible(m_explorerAct->isChecked());
    if (m_terminal->isVisible())
        m_terminalHeight = m_vsplit->sizes().value(1, m_terminalHeight);
    s.setTerminalHeight(m_terminalHeight);
    m_terminal->stop();
    m_serverBar->shutdown();
    m_lsp->shutdown();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (const QUrl &u : event->mimeData()->urls()) {
        if (!u.isLocalFile())
            continue;
        const QFileInfo fi(u.toLocalFile());
        if (fi.isFile())
            m_editors->openFile(fi.absoluteFilePath());
        else if (fi.isDir())
            openProjectPath(fi.absoluteFilePath());
    }
    event->acceptProposedAction();
}

// --- Git ------------------------------------------------------------------------------------------

QString MainWindow::currentFilePath() const
{
    Document *d = m_editors->currentDocument();
    return d && !d->isUntitled() ? d->filePath() : QString();
}

void MainWindow::createGitActions()
{
    using K = Qt::Key;
    const auto C = Qt::CTRL;
    const auto S = Qt::SHIFT;
    const auto A = Qt::ALT;
    auto make = [this](const QString &text, const QKeySequence &key = {}, const QString &icon = {}) {
        auto *a = new QAction(text, this);
        if (!key.isEmpty())
            a->setShortcut(key);
        if (!icon.isEmpty())
            Icons::bind(a, icon);
        return a;
    };
    m_scmAct = make(tr("Version Control"), QKeySequence(C | S | K::Key_G), QStringLiteral(":/new-icons/git-branch.svg"));
    m_gitRefreshAct = make(tr("Refresh Status"));
    m_gitFetchAct = make(tr("Fetch"));
    m_gitPullAct = make(tr("Pull"));
    m_gitPushAct = make(tr("Push"));
    m_gitInitAct = make(tr("Initialize Repository"));
    m_gitNewBranchAct = make(tr("Create Branch…"));
    m_gitStageFileAct = make(tr("Stage Current File"));
    m_gitUnstageFileAct = make(tr("Unstage Current File"));
    m_gitDiscardFileAct = make(tr("Discard Changes in Current File…"));
    m_gitDiffFileAct = make(tr("Show Changes in Current File"));
    m_nextChangeAct = make(tr("Next Change"), QKeySequence(A | K::Key_F3));
    m_prevChangeAct = make(tr("Previous Change"), QKeySequence(S | A | K::Key_F3));
    for (QAction *a : {m_nextChangeAct, m_prevChangeAct}) {
        a->setShortcutContext(Qt::WindowShortcut);
        addAction(a);
    }

    connect(m_scmAct, &QAction::triggered, this, &MainWindow::showSourceControl);
    connect(m_gitRefreshAct, &QAction::triggered, m_git, &GitRepository::refresh);
    connect(m_gitFetchAct, &QAction::triggered, m_git, &GitRepository::fetch);
    connect(m_gitPullAct, &QAction::triggered, m_git, &GitRepository::pull);
    connect(m_gitPushAct, &QAction::triggered, m_git, &GitRepository::push);
    connect(m_gitInitAct, &QAction::triggered, m_git, &GitRepository::init);
    connect(m_gitNewBranchAct, &QAction::triggered, this, [this] { GitPanel::promptNewBranch(m_git, this); });
    connect(m_gitStageFileAct, &QAction::triggered, this, [this] { m_git->stage({currentFilePath()}); });
    connect(m_gitUnstageFileAct, &QAction::triggered, this, [this] { m_git->unstage({currentFilePath()}); });
    connect(m_gitDiscardFileAct, &QAction::triggered, this, [this] { discardPaths({currentFilePath()}); });
    connect(m_gitDiffFileAct, &QAction::triggered, this, &MainWindow::showChangesForCurrentFile);
    connect(m_nextChangeAct, &QAction::triggered, this, [this] { if (auto *e = m_editors->currentEditor()) e->gotoChange(true); });
    connect(m_prevChangeAct, &QAction::triggered, this, [this] { if (auto *e = m_editors->currentEditor()) e->gotoChange(false); });
}

void MainWindow::createGitMenu(QMenu *menu)
{
    menu->addAction(m_scmAct);
    menu->addAction(m_gitRefreshAct);
    menu->addSeparator();
    menu->addAction(m_gitDiffFileAct);
    menu->addAction(m_gitStageFileAct);
    menu->addAction(m_gitUnstageFileAct);
    menu->addAction(m_gitDiscardFileAct);
    menu->addSeparator();
    menu->addAction(m_blameGutterAct);
    menu->addAction(m_blameInlineAct);
    menu->addSeparator();
    menu->addAction(m_nextChangeAct);
    menu->addAction(m_prevChangeAct);
    menu->addSeparator();
    QMenu *branches = menu->addMenu(tr("Switch Branch"));
    connect(branches, &QMenu::aboutToShow, this, [this, branches] {
        branches->clear();
        if (m_git->isRepo())
            GitPanel::populateBranchMenu(m_git, branches, this);
    });
    menu->addAction(m_gitNewBranchAct);
    menu->addSeparator();
    menu->addAction(m_gitFetchAct);
    menu->addAction(m_gitPullAct);
    menu->addAction(m_gitPushAct);
    menu->addSeparator();
    menu->addAction(m_gitInitAct);
}

void MainWindow::setupGit()
{
    connect(m_gitPanel, &GitPanel::openFileRequested, m_editors, &EditorManager::openFile);
    connect(m_gitPanel, &GitPanel::diffRequested, this, &MainWindow::showDiff);

    connect(m_explorer, &ProjectExplorer::gitStageRequested, m_git, &GitRepository::stage);
    connect(m_explorer, &ProjectExplorer::gitUnstageRequested, m_git, &GitRepository::unstage);
    connect(m_explorer, &ProjectExplorer::gitDiscardRequested, this, &MainWindow::discardPaths);
    connect(m_explorer, &ProjectExplorer::gitDiffRequested, this, &MainWindow::showDiff);
    connect(m_explorer, &ProjectExplorer::gitIgnoreRequested, m_git, &GitRepository::addToGitignore);
    connect(m_explorer, &ProjectExplorer::contentsChanged, m_git, &GitRepository::scheduleRefresh);

    connect(m_git, &GitRepository::statusChanged, this, &MainWindow::onGitStatusChanged);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { onGitStatusChanged(); });
    connect(m_git, &GitRepository::repositoryChanged, this, [this] { updateActions(); });
    connect(m_git, &GitRepository::errorOccurred, this, [this](const QString &title, const QString &detail) {
        QMessageBox::warning(this, tr("Git — %1").arg(title), detail);
    });
    connect(m_git, &GitRepository::operationFinished, this, [this](const QString &title, const QString &output) {
        const QString first = output.section(QLatin1Char('\n'), 0, 0).trimmed();
        statusBar()->showMessage(first.isEmpty() ? tr("%1 completed").arg(title) : tr("%1: %2").arg(title, first), 6000);
    });

    // Change markers in the gutter follow the committed text of each open file.
    connect(m_editors, &EditorManager::documentAdded, this, &MainWindow::refreshGutter);
    connect(m_editors, &EditorManager::documentSaved, this, &MainWindow::refreshGutter);
    connect(m_editors, &EditorManager::blameCommitRequested, this, [this](const QString &hash) {
        m_git->commitPatch(hash, this, [this, hash](const QString &text) {
            auto *dlg = new PatchDialog(tr("Commit %1").arg(hash.left(8)), text, this);
            dlg->show();
        });
    });
    connect(m_editors, &EditorManager::documentPathChanged, this, &MainWindow::refreshGutter);
    connect(m_editors, &EditorManager::documentStateChanged, m_git, &GitRepository::scheduleRefresh);

    // Pick up changes made outside the editor (terminal, other tools).
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
        if (st == Qt::ApplicationActive)
            m_git->scheduleRefresh();
    });
    auto *poll = new QTimer(this);
    poll->setInterval(10000);
    connect(poll, &QTimer::timeout, this, [this] {
        if (isActiveWindow() && !m_git->busy())
            m_git->scheduleRefresh();
    });
    poll->start();

    onGitStatusChanged();
}

void MainWindow::onGitStatusChanged()
{
    // Status bar branch indicator
    if (m_git->isRepo()) {
        const QString name = m_git->branch().isEmpty() ? tr("(no branch)") : m_git->branch();
        m_branchButton->setLabel(m_git->isDetached() ? tr("%1 (detached)").arg(name) : name);
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        m_aheadBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-up.svg"), t.textMuted));
        m_behindBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-down.svg"), t.textMuted));
        m_aheadBtn->setText(QString::number(m_git->ahead()));
        m_behindBtn->setText(QString::number(m_git->behind()));
        m_aheadBtn->setVisible(m_git->ahead() > 0);
        m_behindBtn->setVisible(m_git->behind() > 0);
        const int changes = m_git->changes().size();
        m_changesPill->setText(QString::number(changes));
        m_changesPill->setToolTip(tr("%n changed file(s)", nullptr, changes));
        m_changesPill->setVisible(changes > 0);
        m_gitStatusWidget->setVisible(statusBar()->currentMessage().isEmpty()); // a message owns the left side
    } else {
        m_gitStatusWidget->hide();
    }
    const int n = m_git->isRepo() ? m_git->changes().size() : 0;
    m_side->setTitle(1, n > 0 ? tr("Version Control (%1)").arg(n) : tr("Version Control"));

    // Committed text changed (new commit, other branch, other repository) => re-read every open file's base.
    if (m_git->headOid() != m_lastHead || m_git->root() != m_lastRoot) {
        m_lastHead = m_git->headOid();
        m_lastRoot = m_git->root();
        refreshAllGutters();
    }
    updateActions();
}

void MainWindow::refreshAllGutters()
{
    for (Document *d : m_editors->documents())
        refreshGutter(d);
}

void MainWindow::refreshGutter(Document *doc)
{
    CodeEditor *ed = m_editors->editorFor(doc);
    if (!ed)
        return;
    const QString path = doc->filePath();
    const QString rel = path.isEmpty() || !m_git->isRepo() ? QString() : m_git->relativePath(path);
    if (rel.isEmpty() || m_git->isIgnored(path)) {
        ed->clearDiffBase();
        ed->clearBlame();
        return;
    }
    m_git->blame(rel, ed->toPlainText().toUtf8(), ed, [ed](const QVector<GitBlameLine> &lines) {
        if (lines.isEmpty())
            ed->clearBlame();
        else
            ed->setBlame(lines);
    });
    const GitFileChange *c = m_git->changeFor(path);
    const QString headRel = c && !c->origRelPath.isEmpty() ? c->origRelPath : rel;
    m_git->readBlob(QStringLiteral("HEAD:") + headRel, ed, [ed](const GitRepository::Result &r) {
        // A file that is not in HEAD yet is entirely "added".
        ed->setDiffBase(r.ok() ? GitDiff::splitLines(QString::fromUtf8(r.out)) : QStringList());
    });
}

void MainWindow::showSourceControl()
{
    if (!m_explorerAct->isChecked())
        m_explorerAct->setChecked(true);
    m_side->setExpanded(1, true);
}

void MainWindow::showDiff(const QString &path, GitDiffMode mode)
{
    const QString key = QString::number(static_cast<int>(mode)) + path;
    if (DiffDialog *existing = m_diffs.value(key)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dlg = new DiffDialog(m_git, path, mode, this);
    m_diffs.insert(key, dlg);
    dlg->show();
}

void MainWindow::showChangesForCurrentFile()
{
    const QString path = currentFilePath();
    if (path.isEmpty())
        return;
    const GitFileChange *c = m_git->changeFor(path);
    GitDiffMode mode = GitDiffMode::Head;
    if (c)
        mode = c->isStaged() && !c->isUnstaged() ? GitDiffMode::Staged : GitDiffMode::Unstaged;
    showDiff(path, mode);
}

void MainWindow::discardPaths(const QStringList &paths)
{
    QList<GitFileChange> changes;
    for (const QString &p : paths)
        if (const GitFileChange *c = m_git->changeFor(p))
            if (c->isUnstaged() || c->untracked)
                changes << *c;
    if (GitPanel::confirmDiscard(changes, this))
        m_git->discard(paths);
}

// --- Colours, rename, references --------------------------------------------------------------------

Document *MainWindow::documentOf(CodeEditor *editor) const
{
    for (Document *d : m_editors->documents())
        if (m_editors->editorFor(d) == editor)
            return d;
    return nullptr;
}

// Swatches in front of colour literals (CSS colours, Tailwind classes), from every server that knows colours.
void MainWindow::requestColors(Document *doc, CodeEditor *editor)
{
    // Throttle: a server that knows no colours answers nothing, and every status change asks again.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_lsp->isServed(doc) || (!editor->hasColorSwatches() && now - m_colorAsked.value(doc, 0) < 2500))
        return;
    m_colorAsked.insert(doc, now);
    QPointer<CodeEditor> guard(editor);
    m_lsp->documentColors(doc, [this, guard](const QVector<LspColor> &colors) {
        if (!guard)
            return;
        QVector<CodeEditor::ColorSwatch> swatches;
        swatches.reserve(colors.size());
        for (const LspColor &c : colors) {
            CodeEditor::ColorSwatch sw;
            sw.startLine = c.startLine;
            sw.startColumn = c.startColumn;
            sw.endLine = c.endLine;
            sw.endColumn = c.endColumn;
            sw.color = QColor::fromRgbF(qBound(0.0, c.red, 1.0), qBound(0.0, c.green, 1.0), qBound(0.0, c.blue, 1.0), qBound(0.0, c.alpha, 1.0));
            swatches.append(sw);
        }
        m_colorData.insert(guard, colors);
        guard->setColorSwatches(swatches);
    });
}

// Click on a swatch: pick a new colour and write it back in the notation the text already uses.
void MainWindow::pickColor(Document *doc, CodeEditor *editor, int index)
{
    const QVector<LspColor> data = m_colorData.value(editor);
    const QTextCursor range = editor->swatchRange(index);
    if (index < 0 || index >= data.size() || range.isNull())
        return;
    const LspColor lc = data.at(index);
    const QString original = range.selectedText().trimmed().toLower();
    const QColor initial = QColor::fromRgbF(qBound(0.0, lc.red, 1.0), qBound(0.0, lc.green, 1.0), qBound(0.0, lc.blue, 1.0), qBound(0.0, lc.alpha, 1.0));
    const QColor picked = QColorDialog::getColor(initial, this, tr("Pick a Color"), QColorDialog::ShowAlphaChannel);
    if (!picked.isValid())
        return;
    QPointer<CodeEditor> guard(editor);
    m_lsp->colorPresentations(doc, lc, picked, [this, guard, index, original](const QVector<LspColorPresentation> &list) {
        if (!guard)
            return;
        if (list.isEmpty()) {
            statusBar()->showMessage(tr("This colour cannot be edited with the picker"), 4000);
            return;
        }
        // Keep the notation of the original: #hex, rgb(), hsl() ...
        QString prefix;
        for (const QString &p : {QStringLiteral("#"), QStringLiteral("rgb"), QStringLiteral("hsl"), QStringLiteral("hwb")})
            if (original.startsWith(p))
                prefix = p;
        const LspColorPresentation *chosen = &list.first();
        if (!prefix.isEmpty())
            for (const LspColorPresentation &p : list)
                if (p.label.toLower().startsWith(prefix)) {
                    chosen = &p;
                    break;
                }
        guard->replaceSwatch(index, chosen->hasEdit ? chosen->edit.text : chosen->label);
    });
}

// F2: rename the symbol under the caret everywhere the language server knows it.
void MainWindow::renameSymbol()
{
    // F2 belongs to the file tree when it has focus: it renames the selected file or folder
    if (QWidget *f = QApplication::focusWidget(); f && m_explorer->isAncestorOf(f)) {
        m_explorer->renameSelected();
        return;
    }
    CodeEditor *ed = m_editors->currentEditor();
    Document *doc = ed ? documentOf(ed) : nullptr;
    if (!doc)
        return;
    if (!m_lsp->supportsRename(doc)) {
        statusBar()->showMessage(tr("Rename needs a running language server that supports it for this file"), 4000);
        return;
    }
    QTextCursor word = ed->textCursor();
    word.select(QTextCursor::WordUnderCursor);
    QString fallback = word.selectedText();
    // CSS servers rename the whole selector (".card"), so the box shows it with its dot / hash.
    const QChar lead = word.selectionStart() > 0 ? ed->document()->characterAt(word.selectionStart() - 1) : QChar();
    const QString selectorLead = (lead == QLatin1Char('.') || lead == QLatin1Char('#')) && !fallback.isEmpty() ? QString(lead) : QString();
    const bool cssLike = doc->languageName() == QLatin1String("CSS") || doc->languageName() == QLatin1String("SCSS") || doc->languageName() == QLatin1String("Less");
    if (cssLike)
        fallback.prepend(selectorLead);
    const int line = ed->textCursor().blockNumber(), column = ed->textCursor().positionInBlock();
    QPointer<CodeEditor> guard(ed);
    m_lsp->prepareRename(doc, line, column, [this, doc, guard, line, column, fallback, selectorLead, cssLike](bool ok, const QString &placeholder, const QString &error) {
        if (!guard)
            return;
        if (!ok) {
            statusBar()->showMessage(error.isEmpty() ? tr("There is nothing to rename here") : error, 4000);
            return;
        }
        const QString current = placeholder.isEmpty() ? fallback : placeholder;
        if (current.isEmpty()) {
            statusBar()->showMessage(tr("Put the caret on a name to rename it"), 3000);
            return;
        }
        bool accepted = false;
        QString name = QInputDialog::getText(this, tr("Rename Symbol"), tr("New name for “%1”:").arg(current), QLineEdit::Normal, current, &accepted).trimmed();
        if (!accepted || name.isEmpty() || name == current)
            return;
        if (!selectorLead.isEmpty() && cssLike && !name.startsWith(selectorLead))
            name.prepend(selectorLead); // the server replaces the selector including its dot / hash
        m_lsp->rename(doc, line, column, name, [this, current, name](const QJsonObject &edit, const QString &err) {
            if (!err.isEmpty()) {
                statusBar()->showMessage(tr("Rename failed: %1").arg(err), 6000);
                return;
            }
            bool editable = false;
            const QHash<QString, QVector<LspTextEdit>> byPath = LspManager::editsOf(edit, &editable);
            int places = 0;
            for (const QVector<LspTextEdit> &v : byPath)
                places += v.size();
            if (!applyWorkspaceEdit(edit)) {
                statusBar()->showMessage(tr("The language server's rename could not be applied"), 6000);
                return;
            }
            statusBar()->showMessage(tr("Renamed “%1” to “%2”: %3 change(s) in %4 file(s) — files stay unsaved, review with Save All")
                                         .arg(current, name).arg(places).arg(byPath.size()),
                                     8000);
        });
    });
}

// Shift+F12: every use of the symbol under the caret, in a list that jumps to the place.
void MainWindow::findReferences()
{
    CodeEditor *ed = m_editors->currentEditor();
    Document *doc = ed ? documentOf(ed) : nullptr;
    if (!doc)
        return;
    if (!m_lsp->supportsReferences(doc)) {
        statusBar()->showMessage(tr("Find All References needs a running language server that supports it for this file"), 4000);
        return;
    }
    QTextCursor word = ed->textCursor();
    word.select(QTextCursor::WordUnderCursor);
    const QString symbol = word.selectedText();
    QPointer<CodeEditor> guard(ed);
    m_lsp->references(doc, ed->textCursor().blockNumber(), ed->textCursor().positionInBlock(), [this, guard, symbol](const QVector<LspLocation> &locations) {
        if (!guard)
            return;
        if (locations.isEmpty()) {
            statusBar()->showMessage(tr("No references found"), 3000);
            return;
        }
        QHash<QString, QStringList> texts; // file -> lines, read once
        QVector<ReferencesDialog::Entry> entries;
        for (const LspLocation &loc : locations) {
            if (!texts.contains(loc.path)) {
                QString content;
                if (Document *open = m_editors->documentForPath(loc.path)) {
                    content = open->text();
                } else {
                    QFile f(loc.path);
                    if (f.open(QIODevice::ReadOnly))
                        content = QString::fromUtf8(f.read(4 * 1024 * 1024));
                }
                texts.insert(loc.path, content.split(QLatin1Char('\n')));
            }
            const QStringList &lines = texts[loc.path];
            ReferencesDialog::Entry e;
            e.path = loc.path;
            e.line = loc.line;
            e.column = loc.column;
            e.text = loc.line >= 0 && loc.line < lines.size() ? lines.at(loc.line) : QString();
            entries.append(e);
        }
        std::stable_sort(entries.begin(), entries.end(), [](const ReferencesDialog::Entry &a, const ReferencesDialog::Entry &b) {
            return a.path == b.path ? a.line < b.line : a.path < b.path;
        });
        if (!m_refsDialog) {
            m_refsDialog = new ReferencesDialog(this);
            m_refsDialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(m_refsDialog, &ReferencesDialog::openRequested, this,
                    [this](const QString &path, int line, int column) { m_editors->openFileAt(path, line + 1, column + 1); });
        }
        m_refsDialog->setReferences(symbol, entries, m_projects->project().root);
        m_refsDialog->show();
        m_refsDialog->raise();
        m_refsDialog->activateWindow();
    });
}
