#include "MainWindow.h"

#include "Island.h"
#include "SideSections.h"
#include "ai/AiHub.h"
#include "app/InstanceRegistry.h"
#include "app/StartupProfile.h"
#include "cpp/CppTools.h"
#include "debug/DebugPanel.h"
#include "debug/DebugSession.h"
#include "dialogs/FileDialogs.h"
#include "dialogs/NewProjectDialog.h"
#include "dialogs/RunConfigDialog.h"
#include "dialogs/ThemeEditorDialog.h"
#include "editor/Breadcrumbs.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/FileIcons.h"
#include "explorer/ProjectExplorer.h"
#include "expo/ExpoBar.h"
#include "filesystem/FileManager.h"
#include "git/GitPanel.h"
#include "git/GitRepository.h"
#include "gradle/GradlePanel.h"
#include "jvm/GradleProperties.h"
#include "jvm/JvmNewFile.h"
#include "jvm/JvmTools.h"
#include "ktor/KtorBar.h"
#include "lsp/JarSource.h"
#include "lsp/LspManager.h"
#include "media/MarkdownPreview.h"
#include "media/MediaPanel.h"
#include "palette/PalettePopup.h"
#include "platform/Platform.h"
#include "project/JvmProject.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "project/ProjectTemplates.h"
#include "project/PythonEnv.h"
#include "project/QmakeProject.h"
#include "project/VersionCatalog.h"
#include "refactor/MoveController.h"
#include "search/ProjectSearch.h"
#include "search/SearchPanel.h"
#include "settings/SettingsManager.h"
#include "spring/SpringBar.h"
#include "spring/SpringLogView.h"
#include "spring/SpringPanel.h"
#include "spring/SpringProperties.h"
#include "spring/SpringScaffold.h"
#include "tasks/TasksPanel.h"
#include "terminal/ShellSyntax.h"
#include "terminal/TerminalPanel.h"
#include "webdev/DevServerBar.h"
#include "webdev/WebPreview.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setAcceptDrops(true);
    // The window's frame comes first and the panels are built into it. A widget moved under a new parent has its style,
    // font and palette resolved again over its whole subtree, so a layout put together bottom-up (panel, island,
    // splitter, splitter, window) paid that once per level: about 50 ms of every start.
    // Side sections | (chat | editor column) over the terminal | the right-hand island.
    m_hsplit = new QSplitter(Qt::Horizontal, this);
    m_hsplit->setChildrenCollapsible(false);
    m_hsplit->setHandleWidth(8);
    m_hsplit->setContentsMargins(8, 2, 8, 0);
    setCentralWidget(m_hsplit);
    auto *sideIsland = new Island(nullptr);
    m_hsplit->addWidget(sideIsland);
    // The terminal spans the bottom of the agent + editor area, so it never opens the editor column on its own.
    m_vsplit = new QSplitter(Qt::Vertical);
    m_vsplit->setChildrenCollapsible(false);
    m_vsplit->setHandleWidth(8);
    m_hsplit->addWidget(m_vsplit);
    // The agent is the main view; the editor sits in a column on its right that opens with the first file. QODE starts
    // on the editor column alone (the welcome page with the recent projects); the chat opens from the top bar.
    m_workSplit = new QSplitter(Qt::Horizontal);
    m_workSplit->setChildrenCollapsible(false);
    m_workSplit->setHandleWidth(8);
    m_vsplit->addWidget(m_workSplit);
    auto *agentIsland = new Island(nullptr), *editorColumn = new Island(nullptr);
    m_agentIsland = agentIsland;
    m_editorColumn = editorColumn;
    m_editorColumn->setMinimumHeight(60); // otherwise the editor's own minimum stops the terminal from growing tall
    m_workSplit->addWidget(m_agentIsland);
    m_workSplit->addWidget(m_editorColumn);
    m_workSplit->setStretchFactor(0, 1);
    m_workSplit->setStretchFactor(1, 0);
    m_agentIsland->hide();
    auto *terminalIsland = new Island(nullptr);
    m_vsplit->addWidget(terminalIsland);
    m_vsplit->setStretchFactor(0, 1);
    m_vsplit->setStretchFactor(1, 0);
    terminalIsland->hide(); // hidden until Ctrl+J
    // The right-hand island. Its panels (media, Markdown preview, Spring Boot, Ktor, HTTP, web preview, Maven, debugger)
    // are created when first used (mediaPanel() and the like); only the Gradle panel exists from the start, because its
    // Build tab follows every Gradle build run in the terminal, shown or not.
    m_rightStack = new QStackedWidget;
    m_hsplit->addWidget(new Island(m_rightStack));
    m_hsplit->setStretchFactor(0, 0);
    m_hsplit->setStretchFactor(1, 1);
    m_hsplit->setStretchFactor(2, 0);
    m_hsplit->widget(2)->hide(); // shown when an image or video is opened
    StartupProfile::mark(QStringLiteral("window: frame (splitters, islands)"));

    m_projects = new ProjectManager(this);
    m_projectFiles = new ProjectFiles(this);
    m_git = new GitRepository(this);
    m_explorer = new ProjectExplorer(this);
    m_explorer->setNewMenuHook([this](QMenu *menu, const QString &dir) {
        // Java / Kotlin: a class in the folder's package (package folders below src/<set>/java|kotlin, or next to sources).
        if (JvmNewFile::isSourceFolder(dir)) {
            if (JvmNewFile::kotlinFits(dir))
                menu->addAction(FileIcons::forFile(QStringLiteral("x.kt")), tr("New Kotlin Class/File…"), this, [this, dir] { newJvmClass(dir, true); });
            if (JvmNewFile::javaFits(dir) || !JvmNewFile::kotlinFits(dir))
                menu->addAction(FileIcons::forFile(QStringLiteral("x.java")), tr("New Java Class…"), this, [this, dir] { newJvmClass(dir, false); });
        }
        if (!m_springBar || !m_springBar->isAvailable() || SpringScaffold::packageOf(dir, m_springBar->project().sourceRoots()).isEmpty())
            return;
        QMenu *sub = menu->addMenu(tr("New Spring Component"));
        using K = SpringScaffold::Kind;
        sub->addAction(tr("CRUD Set…"), this, [this, dir] { newSpringCrud(dir); });
        sub->addSeparator();
        for (K k : {K::Controller, K::Service, K::Entity, K::Repository, K::Configuration, K::ExceptionHandler})
            sub->addAction(SpringScaffold::title(k), this, [this, k, dir] { newSpringComponent(static_cast<int>(k), dir); });
    });
    m_explorer->setGitRepository(m_git);
    m_gitPanel = new GitPanel(m_git, this);
    m_searchPanel = new SearchPanel(this);
    m_tasks = new TasksPanel(this);
    m_editors = new EditorManager(this);
    editorColumn->setContent(m_editors);
    StartupProfile::mark(QStringLiteral("window: explorer, git, search, tasks, editors"));

    // Left side: collapsible Explorer / Version Control / Search / Tasks sections
    m_side = new SideSections(sideIsland);
    sideIsland->setContent(m_side);
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
    StartupProfile::mark(QStringLiteral("window: side sections"));

    m_terminal = new TerminalPanel(this);
    // Python projects: activate the project's virtualenv (whatever its folder is called) in fresh shells.
    // JVM projects: the project's JDK (JAVA_HOME and PATH), when one was chosen.
    m_terminal->setStartupCommandProvider([this](const QString &shell) {
        QStringList commands;
        if (m_projects->hasProject() && SettingsManager::instance().autoActivateVenv())
            commands << PythonEnv::activationCommand(m_projects->project().root, shell);
        commands << jdkStartupCommand(shell);
        commands.removeAll(QString());
        return ShellSyntax::sequence(ShellSyntax::dialectOf(shell), commands);
    });

    connect(m_terminal, &TerminalPanel::tabsChanged, this, [](const QStringList &names) { SettingsManager::instance().setTerminalTabs(names); });
    m_terminal->setDefaultShell(SettingsManager::instance().terminalShell());
    connect(m_terminal, &TerminalPanel::defaultShellChanged, this, [](const QString &program) { SettingsManager::instance().setTerminalShell(program); });
    terminalIsland->setContent(m_terminal);
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
    StartupProfile::mark(QStringLiteral("window: terminal panel"));

    m_ai = new AiHub(agentIsland);
    agentIsland->setContent(m_ai);
    m_ai->setEditors(m_editors);
    StartupProfile::mark(QStringLiteral("window: QODE chat"));

    m_gradle = new GradlePanel(this);
    m_rightStack->addWidget(m_gradle);
    connect(m_gradle, &GradlePanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_gradle, &GradlePanel::runRequested, this, [this](const QString &command, const QString &dir) {
        if (!m_editors->saveAll())
            return;
        runInTerminal(command, dir);
        m_gradle->showBuild();
    });
    StartupProfile::mark(QStringLiteral("window: Gradle panel"));
    setupHttp();
    setupKtorPanel();
    setupDebugger();
    createJvmActions();
    setupJvm();
    setupMaven();
    StartupProfile::mark(QStringLiteral("window: debugger, JVM, HTTP, Ktor, Maven"));
    connect(m_ai, &AiHub::openFileRequested, this, [this](const QString &path, int line) {
        m_editorHiddenByUser = false; // a file link clicked in the chat
        if (line > 0 ? m_editors->openFileAt(path, line) : m_editors->openFile(path))
            setEditorColumnVisible(true);
    });
    connect(m_ai, &AiHub::projectChanged, this, [this] {
        m_git->scheduleRefresh();
        m_projectFiles->ensureFresh();
    });
    connect(m_ai, &AiHub::gitInitRequested, this, [this] { m_gitInitAct->trigger(); });
    connect(m_ai, &AiHub::claudeCodeRequested, this, [this] {
        if (isMinimized())
            showNormal();
        m_ai->setPage(AiHub::Page::ClaudeCode);
        setAgentVisible(true);
        raise();
        activateWindow();
    });
    wireIdeTools();
    connect(m_projectFiles, &ProjectFiles::updated, this, [this] { m_ai->setProjectFiles(m_projectFiles->files()); });
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(250);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::updatePreview);

    m_lsp = new LspManager(this);
    m_ai->setLsp(m_lsp);
    m_editors->setLspFormatter([this](Document *doc, CodeEditor *ed) {
        auto &cfg = SettingsManager::instance();
        const EditorConfig::Properties &ec = doc->editorConfig(); // .editorconfig wins over the settings
        const int tabSize = ec.indentSize > 0 ? ec.indentSize : ec.tabWidth > 0 ? ec.tabWidth : cfg.tabSize();
        const bool spaces = ec.useTabs >= 0 ? ec.useTabs == 0 : cfg.useSpaces();
        QVector<LspTextEdit> edits;
        if (!m_lsp->formatting(doc, tabSize, spaces, 3000, &edits))
            return false;
        if (!edits.isEmpty())
            ed->applyTextEdits(edits);
        return true;
    });
    m_lsp->setApplyEditHandler([this](const QJsonObject &edit) { return applyWorkspaceEdit(edit); });
    createActions();
    createAssistActions();
    createCppActions();
    createFixActions();
    createJvmEditActions();
    createInlineActions();
    createEditingActions();
    StartupProfile::mark(QStringLiteral("window: actions"));
    createMenus();
    createToolBar();
    createStatusBar();
    StartupProfile::mark(QStringLiteral("window: menus, toolbar, status bar"));

    connect(m_projects, &ProjectManager::projectOpened, this, &MainWindow::onProjectOpened);
    connect(m_projects, &ProjectManager::projectClosed, this, &MainWindow::onProjectClosed);
    connect(m_explorer, &ProjectExplorer::closeProjectRequested, this, &MainWindow::closeProject);
    connect(m_explorer, &ProjectExplorer::fileActivated, m_editors, &EditorManager::openFile);
    connect(m_explorer, &ProjectExplorer::fileCreated, this, [this](const QString &path) {
        if (!m_editors->openFile(path) || !CppTools::isCpp(path))
            return;
        // A new C/C++ file comes with its starter text (#pragma once, a guard, its header's include): start below it.
        if (CodeEditor *ed = m_editors->currentEditor()) {
            const QString text = ed->toPlainText();
            const int guardEnd = text.lastIndexOf(QLatin1String("#endif"));
            QTextCursor c(ed->document());
            c.setPosition(guardEnd >= 2 ? guardEnd - 2 : text.size());
            ed->setTextCursor(c);
        }
    });
    m_mover = new MoveController(m_explorer, m_editors, this);
    connect(m_explorer, &ProjectExplorer::moveRequested, m_mover, &MoveController::move);
    connect(m_explorer, &ProjectExplorer::renameRequested, m_mover, &MoveController::rename);
    connect(m_mover, &MoveController::filesMoved, m_lsp, &LspManager::filesMoved);
    connect(m_explorer, &ProjectExplorer::pathRenamed, m_editors, &EditorManager::pathRenamed);
    connect(m_explorer, &ProjectExplorer::pathDeleted, m_editors, &EditorManager::closeDocumentsUnder);
    connect(m_explorer, &ProjectExplorer::contentsChanged, m_projectFiles, &ProjectFiles::invalidate);
    connect(m_editors, &EditorManager::documentPathChanged, m_projectFiles, &ProjectFiles::invalidate);

    connect(m_editors, &EditorManager::mediaRequested, this, &MainWindow::showMedia);
    connect(m_editors, &EditorManager::previewRequested, this, &MainWindow::togglePreview);
    connect(m_editors, &EditorManager::currentChanged, this, &MainWindow::followPreview);
    connect(m_explorer, &ProjectExplorer::pathDeleted, this, [this](const QString &path) {
        const QString cur = m_media ? m_media->currentPath() : QString();
        if (!cur.isEmpty() && (Platform::samePath(cur, path) || Platform::isUnder(cur, path)))
            hideMedia();
    });
    connect(m_explorer, &ProjectExplorer::pathRenamed, this, [this](const QString &from, const QString &to) {
        const QString cur = m_media ? m_media->currentPath() : QString();
        if (cur.isEmpty())
            return;
        if (cur == from)
            m_media->openMedia(to);
        else if (Platform::isUnder(cur, from))
            m_media->openMedia(to + cur.mid(from.size()));
    });
    connect(m_editors, &EditorManager::newProjectRequested, m_newProjectAct, &QAction::trigger);
    connect(m_editors, &EditorManager::openProjectRequested, m_openProjectAct, &QAction::trigger);
    connect(m_editors, &EditorManager::newFileRequested, m_newFileAct, &QAction::trigger);
    connect(m_editors, &EditorManager::openFileRequested, m_openFileAct, &QAction::trigger);
    // The editor column opens with the first file and closes again with the last one.
    // A file the agent opens while the user keeps the column closed by hand does not reopen it.
    connect(m_editors, &EditorManager::currentChanged, this, [this] {
        if (!m_editors->currentDocument() || m_editorColumn->isVisible())
            return;
        QWidget *focus = QApplication::focusWidget();
        if (m_editorHiddenByUser && focus && m_agentIsland->isAncestorOf(focus))
            return;
        m_editorHiddenByUser = false;
        setEditorColumnVisible(true);
    });
    connect(m_editors, &EditorManager::countChanged, this, [this](int count) {
        if (count == 0)
            setEditorColumnVisible(false);
    });
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
            // License header and doc comments start folded; after the highlighter's first pass and the caret's placement.
            const SettingsManager &st = SettingsManager::instance();
            if (CodeEditor::hasBlockComments(doc->filePath()) && !doc->isLarge() && (st.foldLicenseHeaders() || st.foldDocComments()))
                QTimer::singleShot(0, ed, [ed] {
                    ed->foldComments(SettingsManager::instance().foldLicenseHeaders(), SettingsManager::instance().foldDocComments());
                });
        }
        wireAssists(doc, ed);
        wireHttp(doc, ed);
        wireCpp(doc, ed);
        wireJvm(doc, ed);
        wireJvmEdit(doc, ed);
        wireInline(doc, ed);
        wireEditing(doc, ed);
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
            ed->setLocalCompletions([this, doc](const QString &beforeWord) {
                QVector<LspCompletionItem> items;
                for (const VersionCatalog::Entry &e : VersionCatalog::complete(doc->filePath(), beforeWord)) {
                    LspCompletionItem it;
                    it.label = e.label;
                    it.detail = e.detail;
                    it.kind = e.group ? 9 : 6;
                    items << it;
                }
                // findProperty("…") / providers.gradleProperty("…"): what gradle.properties defines.
                const QString root = m_projects->hasProject() ? m_projects->project().root : QFileInfo(doc->filePath()).absolutePath();
                for (const GradleProperties::Entry &e : GradleProperties::completeInScript(beforeWord, root)) {
                    LspCompletionItem it;
                    it.label = e.label;
                    it.detail = e.detail;
                    it.kind = 10;
                    items << it;
                }
                return items;
            });
        if (SpringProperties::isConfigFile(doc->filePath())) { // spring.* / server.* keys and their values (ktor.* in a Ktor project)
            QPointer<CodeEditor> guard(ed);
            const bool yaml = SpringProperties::isYaml(doc->filePath());
            ed->setLocalCompletions([this, guard, yaml](const QString &beforeWord) {
                QVector<LspCompletionItem> items;
                QStringList parents;
                if (yaml && guard) {
                    QStringList above;
                    const int row = guard->textCursor().blockNumber();
                    for (QTextBlock b = guard->document()->firstBlock(); b.isValid() && b.blockNumber() < row; b = b.next())
                        above << b.text();
                    int indent = 0;
                    while (indent < beforeWord.size() && beforeWord[indent] == QLatin1Char(' '))
                        ++indent;
                    parents = SpringProperties::yamlParents(above, indent);
                }
                for (const SpringProperties::Entry &e : SpringProperties::complete(beforeWord, yaml, parents, ktorAvailable())) {
                    LspCompletionItem it;
                    it.label = e.label;
                    it.detail = e.detail;
                    it.kind = e.group ? 9 : 10;
                    items << it;
                }
                return items;
            });
        }
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
            if (m_dying)
                return;
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
    connect(m_lsp, &LspManager::gradleJdkMissing, this, &MainWindow::offerJdkFix, Qt::QueuedConnection);
    connect(m_editors, &EditorManager::documentPathChanged, m_lsp, &LspManager::documentPathChanged);
    connect(m_lsp, &LspManager::statusChanged, this, &MainWindow::updateLspStatus);
    connect(m_editors, &EditorManager::currentChanged, this, &MainWindow::updateLspStatus); // the button follows the file's server
    // A server that just finished starting: fetch the colour literals of the files already open.
    auto *colorRetry = new QTimer(this);
    colorRetry->setSingleShot(true);
    colorRetry->setInterval(700);
    connect(colorRetry, &QTimer::timeout, this, [this] {
        for (Document *d : m_editors->documents())
            if (CodeEditor *ed = m_editors->editorFor(d))
                if (m_lsp->isServed(d)) {
                    if (!ed->hasColorSwatches())
                        requestColors(d, ed);
                    requestLinks(d, ed);
                }
    });
    connect(m_lsp, &LspManager::statusChanged, colorRetry, qOverload<>(&QTimer::start));
    connect(m_lsp, &LspManager::diagnosticsChanged, this, [this](const QString &path) {
        if (Document *d = m_editors->documentForPath(path))
            if (CodeEditor *ed = m_editors->editorFor(d)) {
                applyDiagnostics(path); // plus kotlinc's errors in Kotlin scripts
                ed->requestAssistsSoon(); // the light bulb follows the diagnostics
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
    StartupProfile::mark(QStringLiteral("window: wiring, settings, git"));
    updateStatus();
    updateActions();
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
    m_launching = true;
    const auto done = qScopeGuard([this] { m_launching = false; });
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
    o.insert(QStringLiteral("agentVisible"), m_agentIsland->isVisible());
    o.insert(QStringLiteral("editorVisible"), m_editorColumn->isVisible());
    o.insert(QStringLiteral("editorWidth"), m_agentIsland->isVisible() && m_editorColumn->isVisible() ? m_workSplit->sizes().value(1) : m_editorColWidth);
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

    m_editorColWidth = o.value(QStringLiteral("editorWidth")).toInt(m_editorColWidth);
    if (!m_launching)
        setAgentVisible(o.value(QStringLiteral("agentVisible")).toBool(false));
    const bool editorShown = o.value(QStringLiteral("editorVisible")).toBool(m_editors->count() > 0);
    m_editorHiddenByUser = !editorShown && m_editors->count() > 0;
    setEditorColumnVisible(editorShown);

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

// Closes this window (saving the session like a normal quit) and starts QODE again once the project claim is released.
void MainWindow::restartApplication()
{
    if (!close())
        return;
    Platform::restartSelf();
}

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
        git.start(Platform::gitExecutable(), {QStringLiteral("init"), QStringLiteral("-q")});
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
    const QString dir = FileDialogs::getExistingDirectory(this, tr("Open Project"), start.isEmpty() ? QDir::homePath() : start);
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
    m_springBar->setProjectRoot(p.root);
    m_ktorBar->setProjectRoot(p.root);
    onExpoChanged();
    m_bookmarks = SettingsManager::instance().bookmarks();
    m_tasks->setBookmarks(m_bookmarks);
    m_breakpoints = SettingsManager::instance().breakpoints(); // before the files reopen: their editors take them from here
    breakpointsEdited(nullptr, nullptr);
    SettingsManager::instance().addRecentProject(p.root);
    m_explorer->setProjectRoot(p.root);
    m_mover->setProjectRoot(p.root);
    m_ai->setProject(p.root);
    m_projectFiles->setRoot(p.root);
    m_editors->setProjectRoot(p.root);
    m_searchPanel->setProjectRoot(p.root);
    m_tasks->setProjectRoot(p.root);
    if (m_media)
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
    m_springBar->setProjectRoot({});
    m_ktorBar->setProjectRoot({});
    m_metroStarted = false;
    onExpoChanged();
    m_bookmarks.clear();
    m_tasks->setBookmarks(m_bookmarks);
    m_breakpoints.clear();
    if (m_debugPanel)
        m_debugPanel->setBreakpoints({});
    m_explorer->setProjectRoot({});
    m_mover->setProjectRoot({});
    m_ai->setProject({});
    m_projectFiles->setRoot({});
    m_editors->setProjectRoot({});
    m_searchPanel->setProjectRoot({});
    m_tasks->setProjectRoot({});
    hideMedia();
    if (m_media)
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
    const QStringList files = FileDialogs::getOpenFileNames(this, tr("Open File"), start.isEmpty() ? QDir::homePath() : start);
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
    const bool cpp = hasDoc && CppTools::isCpp(m_editors->currentDocument()->filePath());
    m_switchHeaderAct->setEnabled(cpp);
    m_generateDefAct->setEnabled(cpp);
    m_copyRefAct->setEnabled(hasDoc && !m_editors->currentDocument()->filePath().isEmpty());
    m_inlineEditAct->setEnabled(hasDoc);
    m_copyImageAct->setEnabled(hasDoc);
    if ((m_serverBar && m_serverBar->isAvailable()) || (m_expoBar && m_expoBar->isAvailable()) || (m_springBar && m_springBar->isAvailable())
        || ktorAvailable()) {
        m_runAct->setEnabled(true);
        m_runConfigAct->setEnabled(true);
    } else {
        const bool project = m_projects->hasProject();
        m_runAct->setEnabled(project || !currentFilePath().isEmpty() || (hasDoc && m_editors->currentDocument()->isUntitled()));
        m_runConfigAct->setEnabled(project || !currentFilePath().isEmpty());
    }

    // Debugging is for the JVM: a Kotlin / Java / Kotlin script file, or a Gradle / Maven project.
    {
        const QString file = currentFilePath();
        const bool jvmFile = file.endsWith(QLatin1String(".kt")) || file.endsWith(QLatin1String(".java")) || JvmTools::isKotlinScript(file);
        m_debugAct->setVisible(jvmFile || (m_projects->hasProject() && JvmTools::isJvmBuild(m_projects->root())) || m_debug->isActive());
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
    m_posLabel->setText(e->cursorCount() > 1 ? tr("Ln %1, Col %2 (%n carets)", nullptr, e->cursorCount()).arg(e->currentLine()).arg(e->currentColumn())
                                             : tr("Ln %1, Col %2").arg(e->currentLine()).arg(e->currentColumn()));
    m_modeLabel->setText(e->overwriteMode() ? tr("OVR") : tr("INS"));
}

// The chat area has two pages, QODE chat and Claude Code: each button shows its page, or hides the area when its page
// is already showing.
void MainWindow::toggleAiPanel()
{
    if (m_agentIsland->isVisible() && m_ai->page() != AiHub::Page::Qode) {
        m_ai->setPage(AiHub::Page::Qode);
        setAgentVisible(true);
        return;
    }
    if (!m_agentIsland->isVisible())
        m_ai->setPage(AiHub::Page::Qode);
    setAgentVisible(!m_agentIsland->isVisible());
}

void MainWindow::toggleClaudeCode()
{
    if (m_agentIsland->isVisible() && m_ai->page() != AiHub::Page::ClaudeCode) {
        m_ai->setPage(AiHub::Page::ClaudeCode);
        setAgentVisible(true);
        return;
    }
    if (!m_agentIsland->isVisible())
        m_ai->setPage(AiHub::Page::ClaudeCode);
    setAgentVisible(!m_agentIsland->isVisible());
}

void MainWindow::showMcpServers()
{
    m_ai->showMcpServers();
}

// Hiding the agent leaves the classic editor layout; at least one of agent and editor column is always shown.
void MainWindow::setAgentVisible(bool show)
{
    m_aiAct->setChecked(show && m_ai->page() == AiHub::Page::Qode);
    m_claudeCodeAct->setChecked(show && m_ai->page() == AiHub::Page::ClaudeCode);
    if (show == m_agentIsland->isVisible()) {
        if (show)
            m_ai->focusInput();
        return;
    }
    if (show) {
        m_agentIsland->show();
        if (m_editorColumn->isVisible()) {
            const int total = m_workSplit->width();
            const int w = qBound(320, m_editorColWidth > 0 ? m_editorColWidth : total * 45 / 100, qMax(320, total - 360));
            m_workSplit->setSizes({total - w, w});
            if (m_editors->count() == 0)
                setEditorColumnVisible(false);
        }
        m_projectFiles->ensureFresh();
        m_ai->setProjectFiles(m_projectFiles->files());
        m_ai->focusInput();
        return;
    }
    if (m_editorColumn->isVisible())
        m_editorColWidth = m_workSplit->sizes().value(1, m_editorColWidth);
    else
        setEditorColumnVisible(true);
    m_agentIsland->hide();
    m_editors->focusEditor();
}

void MainWindow::setEditorColumnVisible(bool show)
{
    m_editorPaneAct->setChecked(show || !m_agentIsland->isVisible());
    if (show == m_editorColumn->isVisible())
        return;
    if (!show) {
        if (!m_agentIsland->isVisible())
            return; // the editor is all there is
        m_editorColWidth = m_workSplit->sizes().value(1, m_editorColWidth);
        m_editorColumn->hide();
        m_editorPaneAct->setChecked(false);
        return;
    }
    m_editorColumn->show();
    if (m_agentIsland->isVisible()) {
        const int total = m_workSplit->width();
        const int w = qBound(320, m_editorColWidth > 0 ? m_editorColWidth : total * 45 / 100, qMax(320, total - 360));
        m_workSplit->setSizes({total - w, w});
    }
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
    if (m_serverBar->isAvailable() && !springOwns(currentFilePath()) && !ktorOwns(currentFilePath())) {
        m_serverBar->configure();
        return;
    }
    if (m_springBar->isAvailable()) {
        m_springBar->configure();
        return;
    }
    if (ktorAvailable()) {
        m_ktorBar->configure();
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
    // A Kotlin script runs itself (kotlinc -script, or ./script.kts with a shebang), wherever it lives.
    if (Document *d = m_editors->currentDocument(); d && JvmTools::isKotlinScript(d->filePath())
        && SettingsManager::instance().runCommand(RunConfigDialog::keyFor(d->filePath())).isEmpty()) {
        runKotlinScript(d, false);
        return;
    }
    // In a Spring Boot project F5 means the application, unless this file type has a command of its own.
    if (springOwns(currentFilePath())) {
        const QString file = currentFilePath();
        if (file.isEmpty() || SettingsManager::instance().runCommand(RunConfigDialog::keyFor(file)).isEmpty()) {
            if (!m_editors->saveAll())
                return;
            m_springBar->start(); // restarts when it is already running
            return;
        }
    }
    // Same for a Ktor server.
    if (ktorOwns(currentFilePath())) {
        const QString file = currentFilePath();
        if (file.isEmpty() || SettingsManager::instance().runCommand(RunConfigDialog::keyFor(file)).isEmpty()) {
            if (!m_editors->saveAll())
                return;
            m_ktorBar->start();
            return;
        }
    }
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
        runInTerminal(RunConfigDialog::expand(projectCmd, path, root));
        return;
    }
    if (doc && doc->isModified() && !m_editors->saveCurrent())
        return;
    runInTerminal(RunConfigDialog::expand(s.runCommand(RunConfigDialog::keyFor(path)), path, root));
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
        if (m_editors->count() > 0 && m_editorColumn->isVisible())
            m_editors->focusEditor();
        else
            m_ai->focusInput();
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
        if (const qsizetype tab = it.title.indexOf(QLatin1Char('\t')); tab >= 0) { // "Add Next Occurrence\tAlt+J": keys the editor handles
            if (it.hint.isEmpty())
                it.hint = it.title.mid(tab + 1);
            it.title.truncate(tab);
        }
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
            if (!root.isEmpty() && (Platform::samePath(dir, root) || Platform::isUnder(dir, root)))
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
    CodeEditor *ed = m_editors->editorFor(doc);
    if (ed && ed->hasDocumentSymbols() && m_lsp->isServed(doc)) {
        // The language server's declarations (nested members with their owner); kept fresh by CodeEditor::serverDataRequested.
        for (const Breadcrumbs::ServerSymbol &sym : Breadcrumbs::flattenSymbols(ed->documentSymbols())) {
            PalettePopup::Item it;
            it.title = sym.name;
            it.detail = sym.parent.isEmpty() ? sym.kind : QStringLiteral("%1 · %2").arg(sym.kind, sym.parent);
            it.hint = QString::number(sym.line + 1);
            it.data = sym.line + 1;
            items.append(it);
        }
    } else {
        for (const Breadcrumbs::Symbol &sym : Breadcrumbs::documentSymbols(doc->textDocument(), doc->languageName())) {
            PalettePopup::Item it;
            it.title = sym.name;
            it.detail = sym.parent;
            it.hint = QString::number(sym.line + 1);
            it.data = sym.line + 1;
            items.append(it);
        }
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
                               .arg(QCoreApplication::applicationVersion())
                           // Logos shown in the file icons and panels; the notices must stay.
                           + QStringLiteral("<p><small>Copyright © 2026 JetBrains s.r.o. Kotlin and the Kotlin logo are trademarks of JetBrains s.r.o.<br>"
                                            "Copyright © 2026 JetBrains s.r.o. Ktor and the Ktor logo are trademarks of JetBrains s.r.o.</small></p>"));
}

void MainWindow::showStartupTime()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Startup Time"));
    auto *text = new QPlainTextEdit(StartupProfile::report(), &dlg);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(SettingsManager::instance().editorFont());
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *intro = new QLabel(tr("Where QODE's start went, from main() to the first frame on screen. Panels made when first "
                                "used are listed below it. QODE_STARTUP_PROFILE=1 prints this to the terminal."),
                             &dlg);
    intro->setWordWrap(true);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(intro);
    layout->addWidget(text, 1);
    layout->addWidget(buttons);
    dlg.resize(620, 560);
    dlg.exec();
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
    m_expoBar->shutdown();
    m_springBar->shutdown();
    m_ktorBar->shutdown();
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

Document *MainWindow::documentOf(CodeEditor *editor) const
{
    for (Document *d : m_editors->documents())
        if (m_editors->editorFor(d) == editor)
            return d;
    return nullptr;
}
