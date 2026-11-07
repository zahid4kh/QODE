// MainWindow's actions, menus, toolbar and status bar (created once, from the constructor).
#include "MainWindow.h"

#include "SideSections.h"
#include "dialogs/QodeFilesDialog.h"
#include "editor/Breadcrumbs.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "editor/TextEncoding.h"
#include "editor/ZoomPopup.h"
#include "explorer/ProjectExplorer.h"
#include "expo/ExpoBar.h"
#include "expo/ExpoSchema.h"
#include "expo/ExpoWebServer.h"
#include "filesystem/FileManager.h"
#include "git/BranchButton.h"
#include "git/BranchPopup.h"
#include "git/GitRepository.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "settings/ThemeManager.h"
#include "spring/SpringBar.h"
#include "spring/SpringPanel.h"
#include "terminal/TerminalPanel.h"
#include "webdev/DevServer.h"
#include "webdev/DevServerBar.h"
#include "webdev/WebPreview.h"

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>

#include <functional>

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
    m_aiAct = make(tr("QODE Chat"), QKeySequence(C | A | K::Key_G), QStringLiteral(":/new-icons/sparkles.svg"));
    m_aiAct->setToolTip(tr("QODE chat: Gemini or Claude explores the project, edits files, runs commands and checks its work (Ctrl+Alt+G)"));
    m_aiAct->setCheckable(true);
    m_claudeCodeAct = make(tr("Claude Code"), QKeySequence(C | A | K::Key_C));
    m_claudeCodeAct->setIcon(Icons::tinted(QStringLiteral(":/new-icons/claudecode.svg"), Theme::claudeCode())); // the same in every theme
    m_claudeCodeAct->setToolTip(tr("Claude Code: the claude CLI in a window, working in this project (Ctrl+Alt+C)"));
    m_claudeCodeAct->setCheckable(true);
    m_mcpAct = make(tr("MCP Servers…"), {}, QStringLiteral(":/new-icons/server.svg"));
    m_mcpAct->setToolTip(tr("MCP servers: tools from other programs (Blender, databases, …) for the QODE chat"));
    m_editorPaneAct = make(tr("Editor Panel"), QKeySequence(C | A | K::Key_E), QStringLiteral(":/new-icons/panel-right.svg"));
    m_editorPaneAct->setToolTip(tr("Show or hide the editor and terminal beside the QODE chat (Ctrl+Alt+E)"));
    m_editorPaneAct->setCheckable(true);
    m_editorPaneAct->setChecked(true); // QODE starts on the editor column (the welcome page)
    m_gradleAct = make(tr("Gradle Tasks"), {}, QStringLiteral(":/new-icons/gradle.svg"));
    m_gradleAct->setToolTip(tr("Show the Gradle tasks of this project (build, test, package, …) and run them"));
    m_mavenAct = make(tr("Maven"), {}, QStringLiteral(":/new-icons/apachemaven.svg"));
    m_mavenAct->setToolTip(tr("Show the Maven panel: lifecycle phases, plugin goals, profiles, dependency tree and effective POM"));
    m_mavenAct->setVisible(false); // only in Maven projects (updateMavenUi)
    m_springAct = make(tr("Spring Boot"), {}, QStringLiteral(":/new-icons/springboot.svg"));
    m_springAct->setToolTip(tr("Show the Spring Boot panel: application status and live log"));
    m_springAct->setVisible(false); // only in Spring Boot projects (updateSpringUi)
    m_springStarterAct = make(tr("Add Spring Starters…"), {}, QStringLiteral(":/new-icons/package.svg"));
    m_springStarterAct->setToolTip(tr("Add Web, JPA, Security, Validation, Actuator … dependencies to the build file"));
    m_springStarterAct->setVisible(false);
    m_ktorAct = make(tr("Ktor"), {}, QStringLiteral(":/new-icons/ktor.svg"));
    m_ktorAct->setToolTip(tr("Show the Ktor panel: server status, live log, routes, requests and configuration"));
    m_ktorAct->setVisible(false); // only in Ktor projects (updateKtorUi)
    m_ktorPluginAct = make(tr("Add Ktor Plugins…"), {}, QStringLiteral(":/new-icons/package.svg"));
    m_ktorPluginAct->setToolTip(tr("Add CORS, JSON, authentication, WebSockets … modules to the build file"));
    m_ktorPluginAct->setVisible(false);
    m_httpAct = make(tr("HTTP Client"), {}, QStringLiteral(":/new-icons/arrow-left-right.svg"));
    m_httpAct->setToolTip(tr("Show the requests of the open .http / .rest file and their responses"));
    m_newHttpAct = make(tr("New HTTP Request File…"), {}, QStringLiteral(":/new-icons/arrow-left-right.svg"));
    m_newHttpAct->setToolTip(tr("Create a .http file: requests you send with Ctrl+Enter (VS Code REST Client / IntelliJ format)"));
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
    m_zoomInAct = make(tr("Zoom In"), QKeySequence(QKeySequence::ZoomIn));
    m_zoomInAct->setShortcuts({QKeySequence(QStringLiteral("Ctrl+=")), QKeySequence(QStringLiteral("Ctrl++"))});
    m_zoomOutAct = make(tr("Zoom Out"), QKeySequence(QStringLiteral("Ctrl+-")));
    m_zoomResetAct = make(tr("Reset Zoom"), QKeySequence(QStringLiteral("Ctrl+0")));
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
    m_predictAct = make(tr("Next Edit Predictions (Tab)"));
    m_predictAct->setCheckable(true);
    m_predictAct->setChecked(SettingsManager::instance().editPredictions());
    m_predictAct->setToolTip(tr("After a rename or a parameter change, Tab applies the same change where else it belongs"));
    m_indentGuidesAct = make(tr("Indent Guides"));
    m_indentGuidesAct->setCheckable(true);
    m_indentGuidesAct->setChecked(SettingsManager::instance().indentGuides());
    m_nextTabAct = make(tr("Next Tab"), QKeySequence(C | K::Key_Tab));
    m_prevTabAct = make(tr("Previous Tab"), QKeySequence(C | S | K::Key_Backtab));
    m_splitRightAct = make(tr("Split Editor Right"), QKeySequence(C | K::Key_Backslash));
    m_splitDownAct = make(tr("Split Editor Down"), QKeySequence(C | Qt::ALT | K::Key_Backslash));
    m_aboutAct = make(tr("About QODE"));
    m_paletteAct = make(tr("Command Palette…"), QKeySequence(C | S | K::Key_P), QStringLiteral(":/new-icons/search.svg"));
    m_quickOpenAct = make(tr("Go to File…"), QKeySequence(C | K::Key_E), QStringLiteral(":/new-icons/file-input.svg"));
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
    connect(m_mavenAct, &QAction::triggered, this, &MainWindow::toggleMavenPanel);
    connect(m_springAct, &QAction::triggered, this, &MainWindow::toggleSpringPanel);
    connect(m_springStarterAct, &QAction::triggered, this, &MainWindow::addSpringStarters);
    connect(m_ktorAct, &QAction::triggered, this, &MainWindow::toggleKtorPanel);
    connect(m_ktorPluginAct, &QAction::triggered, this, &MainWindow::addKtorPlugins);
    connect(m_httpAct, &QAction::triggered, this, &MainWindow::toggleHttpPanel);
    connect(m_newHttpAct, &QAction::triggered, this, &MainWindow::newHttpFile);
    connect(m_aiAct, &QAction::triggered, this, &MainWindow::toggleAiPanel);
    connect(m_claudeCodeAct, &QAction::triggered, this, &MainWindow::toggleClaudeCode);
    connect(m_mcpAct, &QAction::triggered, this, &MainWindow::showMcpServers);
    connect(m_editorPaneAct, &QAction::triggered, this, [this] {
        const bool show = !m_editorColumn->isVisible();
        m_editorHiddenByUser = !show;
        setEditorColumnVisible(show);
        if (show)
            m_editors->focusEditor();
    });
    connect(m_fullscreenAct, &QAction::triggered, this, [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
    connect(m_zoomInAct, &QAction::triggered, this, [] { auto &s = SettingsManager::instance(); s.setUiZoom(s.uiZoom() + 10); });
    connect(m_zoomOutAct, &QAction::triggered, this, [] { auto &s = SettingsManager::instance(); s.setUiZoom(s.uiZoom() - 10); });
    connect(m_zoomResetAct, &QAction::triggered, this, [] { SettingsManager::instance().setUiZoom(100); });
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
    connect(m_predictAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setEditPredictions(on); });
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
    file->addAction(m_newKotlinAct);
    file->addAction(m_newJavaAct);
    file->addAction(m_newHttpAct);
    file->addMenu(m_scratchMenu);
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
    saveOptions->addAction(m_fixOnSaveAct);
    saveOptions->addAction(m_organizeOnSaveAct);
    file->addAction(m_localHistoryAct);
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
    QMenu *cursors = edit->addMenu(tr("Multiple Cursors"));
    cursors->addActions(m_cursorActs);
    cursors->insertSeparator(m_cursorActs.value(3));
    cursors->insertSeparator(m_cursorActs.value(6));
    edit->addSeparator();
    edit->addAction(m_formatAct);
    QMenu *sourceMenu = edit->addMenu(tr("Source Actions"));
    sourceMenu->addAction(m_organizeImportsAct);
    sourceMenu->addAction(m_removeUnusedAct);
    sourceMenu->addAction(m_addMissingAct);
    sourceMenu->addAction(m_fixAllAct);
    edit->addAction(m_inlineEditAct);
    edit->addAction(m_quickFixAct);
    edit->addAction(m_generateDefAct);
    edit->addAction(m_signatureAct);
    edit->addAction(m_quickInfoAct);
    edit->addSeparator();
    edit->addAction(m_findAct);
    edit->addAction(m_replaceAct);
    edit->addAction(m_gotoLineAct);
    edit->addAction(m_gotoSymbolAct);
    edit->addAction(m_workspaceSymbolAct);
    edit->addAction(m_gotoDefinitionAct);
    edit->addAction(m_switchHeaderAct);
    edit->addAction(m_copyRefAct);
    edit->addAction(m_copyImageAct);
    edit->addAction(m_typeDefAct);
    edit->addAction(m_implAct);
    edit->addAction(m_findRefsAct);
    edit->addAction(m_callHierarchyAct);
    edit->addAction(m_typeHierarchyAct);
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
    view->addAction(m_mavenAct);
    view->addAction(m_springAct);
    view->addAction(m_springStarterAct);
    view->addAction(m_ktorAct);
    view->addAction(m_ktorPluginAct);
    view->addAction(m_httpAct);
    view->addAction(m_aiAct);
    view->addAction(m_claudeCodeAct);
    view->addAction(m_mcpAct);
    view->addAction(m_editorPaneAct);
    view->addSeparator();
    view->addAction(m_splitRightAct);
    view->addAction(m_splitDownAct);
    view->addSeparator();
    view->addAction(m_wordWrapAct);
    view->addAction(m_zoomInAct);
    view->addAction(m_zoomOutAct);
    view->addAction(m_zoomResetAct);
    view->addAction(m_previewAct);
    view->addAction(m_showBookmarksAct);
    view->addAction(m_showTodosAct);
    view->addSeparator();
    view->addAction(m_indentGuidesAct);
    view->addAction(m_stickyAct);
    view->addAction(m_predictAct);
    view->addAction(m_minimapAct);
    QMenu *foldMenu = view->addMenu(tr("Folding"));
    QAction *foldHeaderAct = foldMenu->addAction(tr("Fold License Headers When Opening Files"));
    foldHeaderAct->setCheckable(true);
    foldHeaderAct->setChecked(SettingsManager::instance().foldLicenseHeaders());
    foldHeaderAct->setToolTip(tr("The comment block above a file's first code starts folded"));
    connect(foldHeaderAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setFoldLicenseHeaders(on); });
    QAction *foldDocsAct = foldMenu->addAction(tr("Fold Doc Comments When Opening Files"));
    foldDocsAct->setCheckable(true);
    foldDocsAct->setChecked(SettingsManager::instance().foldDocComments());
    foldDocsAct->setToolTip(tr("KDoc / Javadoc / JSDoc comments (/** … */) of three lines or more start folded"));
    connect(foldDocsAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setFoldDocComments(on); });
    foldMenu->setToolTipsVisible(true);
    QMenu *hintsMenu = view->addMenu(tr("Inlay Hints"));
    hintsMenu->addAction(m_hintParamsAct);
    hintsMenu->addAction(m_hintTypesAct);
    view->addAction(m_semanticAct);
    view->addAction(m_symbolHighlightAct);
    view->addAction(m_lightBulbAct);
    view->addAction(m_codeLensAct);
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

    QMenu *run = menuBar()->addMenu(tr("&Run"));
    run->addAction(m_runAct);
    run->addAction(m_runConfigAct);
    run->addSeparator();
    run->addAction(m_debugAct);
    run->addAction(m_resumeAct);
    run->addAction(m_pauseAct);
    run->addAction(m_stepOverAct);
    run->addAction(m_stepIntoAct);
    run->addAction(m_stepOutAct);
    run->addAction(m_stopDebugAct);
    run->addSeparator();
    run->addAction(m_breakpointAct);
    run->addAction(m_debugPanelAct);

    QMenu *project = menuBar()->addMenu(tr("&Project"));
    project->addAction(m_projNewFileAct);
    project->addAction(m_projNewFolderAct);
    project->addSeparator();
    project->addAction(m_dependenciesAct);
    project->addAction(m_depUpdatesAct);
    project->addAction(m_jdkAct);
    project->addSeparator();
    project->addAction(m_openProjectFolderAct);
    project->addAction(m_closeProjectAct);

    createGitMenu(menuBar()->addMenu(tr("&Git")));
    createLspMenu(menuBar()->addMenu(tr("&LSP")));

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("QODE's Files…"), this, [this] { (new QodeFilesDialog(this))->open(); });
    help->addAction(tr("Startup Time…"), this, &MainWindow::showStartupTime);
    help->addSeparator();
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
    tb->addAction(m_aiAct);
    tb->addAction(m_claudeCodeAct);
    tb->addAction(m_editorPaneAct);
    tb->addSeparator();
    m_mainToolBar = tb;
    // A web project runs its dev server instead of single files: the server controls replace the two run buttons.
    m_serverBar = new DevServerBar(this);
    m_serverBarAct = tb->addWidget(m_serverBar);
    connect(m_serverBar, &DevServerBar::availabilityChanged, this, [this] {
        updateRunToolbar();
        updateActions();
        updatePreviewTargets();
    });
    connect(m_serverBar, &DevServerBar::stateChanged, this, &MainWindow::updateActions);
    connect(m_serverBar, &DevServerBar::previewRequested, this, [this] {
        if (rightPanelShows(m_webPreview) && m_webPreview->currentTarget() == QLatin1String("web"))
            hideMedia();
        else
            showPreviewTarget(QStringLiteral("web"), false);
    });
    connect(m_serverBar, &DevServerBar::stateChanged, this, [this] {
        // A starting server brings up its live preview, but never replaces another open panel.
        if (m_serverBar->server()->state() == DevServer::State::Starting && !m_rightStack->parentWidget()->isVisible())
            showPreviewTarget(QStringLiteral("web"), false);
    });
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
    connect(m_expoBar, &ExpoBar::previewRequested, this, [this] {
        const ExpoApp *app = m_expoBar->currentApp();
        if (!app)
            return;
        const QString id = QStringLiteral("expo:") + app->rel;
        if (rightPanelShows(m_webPreview) && m_webPreview->currentTarget() == id)
            hideMedia();
        else
            showPreviewTarget(id, true);
    });
    connect(m_expoBar->webServer(), &DevServer::stateChanged, this, [this] {
        updatePreviewTargets(); // which app the preview Metro runs
        if (m_expoBar->webServer()->state() == DevServer::State::Starting && !m_rightStack->parentWidget()->isVisible())
            toggleWebPreview();
    });
    connect(m_expoSchema, &ExpoSchema::ready, this, &MainWindow::pushExpoSchemas);
    connect(m_expoSchema, &ExpoSchema::failed, this, [this](const QString &, const QString &why) { statusBar()->showMessage(why, 8000); });
    // A Spring Boot application: status, start / stop and the panel with its log.
    m_springBar = new SpringBar(this);
    m_springBarAct = tb->addWidget(m_springBar);
    connect(m_springBar, &SpringBar::availabilityChanged, this, [this] {
        updateRunToolbar();
        updateActions();
        updateSpringUi();
        updateKtorUi(); // Spring Boot wins over Ktor
    });
    connect(m_springBar, &SpringBar::stateChanged, this, &MainWindow::updateActions);
    connect(m_springBar, &SpringBar::panelRequested, this, &MainWindow::toggleSpringPanel);
    connect(m_springBar, &SpringBar::openFileRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_springBar, &SpringBar::starting, this, [this] {
        if (!m_rightStack->parentWidget()->isVisible())
            toggleSpringPanel(); // show what the application prints, but never replace another open panel
    });
    setupKtorBar(tb);
    connect(m_serverBar, &DevServerBar::openFileRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_serverBar, &DevServerBar::nestedProjectsFound, this, [this](const QStringList &dirs) {
        // After the project finished opening, so the question does not hold up restoring the session.
        QTimer::singleShot(0, this, [this, dirs] { askNestedWebProject(dirs); });
    });
    updateRunToolbar();
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
    m_lspProgress = new QProgressBar(this);
    m_lspProgress->setTextVisible(false);
    m_lspProgress->setFixedSize(70, 6);
    m_lspProgress->hide();
    statusBar()->addPermanentWidget(m_lspProgress);
    m_lspButton = new QToolButton(this);
    m_lspButton->setAutoRaise(true);
    m_lspButton->setPopupMode(QToolButton::InstantPopup);
    m_lspButton->setCursor(Qt::PointingHandCursor);
    m_lspButton->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 6px; } QToolButton::menu-indicator { image: none; }"));
    m_lspButton->setMenu(m_lspMenu); // the same menu as LSP in the menu bar (created first)
    statusBar()->addPermanentWidget(m_lspButton);
    m_zoomButton = new QToolButton(this);
    m_zoomButton->setAutoRaise(true);
    m_zoomButton->setCursor(Qt::PointingHandCursor);
    m_zoomButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_zoomButton->setIconSize(QSize(14, 14));
    m_zoomButton->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 6px; }"));
    connect(m_zoomButton, &QToolButton::clicked, this, [this] { ZoomPopup::open(m_zoomButton); });
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, [this] { updateZoomButton(); });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { updateZoomButton(); });
    connect(&SettingsManager::instance(), &SettingsManager::projectSettingsChanged, this, [this] { updateZoomButton(); });
    statusBar()->addPermanentWidget(m_zoomButton);
    updateZoomButton();
    m_langLabel = mk(90);
    m_encLabel = mk(70);
    m_eolLabel = mk(40);
    // Both open a menu on a click: the encoding (Reopen / Save with Encoding) and the line endings (LF / CRLF).
    struct Click : QObject {
        std::function<void()> run;
        Click(QObject *parent, std::function<void()> f) : QObject(parent), run(std::move(f)) {}
        bool eventFilter(QObject *, QEvent *e) override
        {
            if (e->type() != QEvent::MouseButtonRelease)
                return false;
            run();
            return true;
        }
    };
    m_encLabel->setCursor(Qt::PointingHandCursor);
    m_encLabel->setToolTip(tr("Encoding: click to reopen or save the file in another one"));
    m_encLabel->installEventFilter(new Click(m_encLabel, [this] { showEncodingMenu(); }));
    m_eolLabel->setCursor(Qt::PointingHandCursor);
    m_eolLabel->setToolTip(tr("Line endings: click to switch between LF and CRLF"));
    m_eolLabel->installEventFilter(new Click(m_eolLabel, [this] { showLineEndingMenu(); }));
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

void MainWindow::showEncodingMenu()
{
    Document *d = m_editors->currentDocument();
    if (!d)
        return;
    QMenu menu(this);
    QMenu *reopen = menu.addMenu(tr("Reopen with Encoding"));
    QMenu *save = menu.addMenu(tr("Save with Encoding"));
    reopen->setEnabled(!d->isUntitled());
    for (const QString &enc : TextEncoding::names()) {
        QAction *r = reopen->addAction(enc);
        r->setCheckable(true);
        r->setChecked(enc == d->encoding());
        connect(r, &QAction::triggered, this, [this, d, enc] {
            if (d->isModified()
                && QMessageBox::question(this, tr("Reopen with Encoding"),
                                         tr("Reading \"%1\" again as %2 drops its unsaved changes. Go on?").arg(d->fileName(), enc))
                       != QMessageBox::Yes)
                return;
            QString error;
            if (!d->reopenWithEncoding(enc, &error))
                QMessageBox::warning(this, tr("Reopen with Encoding"), error);
            updateStatus();
        });
        QAction *s = save->addAction(enc);
        s->setCheckable(true);
        s->setChecked(enc == d->encoding());
        connect(s, &QAction::triggered, this, [this, d, enc] {
            d->setEncoding(enc);
            QString error;
            if (!d->isUntitled() && !d->save(&error))
                QMessageBox::warning(this, tr("Save with Encoding"), error);
            updateStatus(); // a legacy encoding that cannot hold the text was saved as UTF-8 instead
        });
    }
    menu.exec(m_encLabel->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu.sizeHint().height()));
}

void MainWindow::showLineEndingMenu()
{
    Document *d = m_editors->currentDocument();
    if (!d)
        return;
    QMenu menu(this);
    for (const auto &[ending, label] : {std::pair{Document::LineEnding::LF, tr("LF (Linux, macOS)")},
                                        std::pair{Document::LineEnding::CRLF, tr("CRLF (Windows)")}}) {
        QAction *a = menu.addAction(label);
        a->setCheckable(true);
        a->setChecked(d->lineEnding() == ending);
        connect(a, &QAction::triggered, this, [this, d, e = ending] {
            d->setLineEnding(e); // written on the next save; the document shows as modified
            updateStatus();
        });
    }
    menu.exec(m_eolLabel->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu.sizeHint().height()));
}

void MainWindow::updateZoomButton()
{
    const int z = SettingsManager::instance().uiZoom();
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_zoomButton->setIcon(Icons::tinted(z < 100 ? QStringLiteral(":/new-icons/zoom-out.svg") : QStringLiteral(":/new-icons/zoom-in.svg"), t.textMuted));
    m_zoomButton->setText(QStringLiteral("%1%").arg(z));
    m_zoomButton->setToolTip(tr("Zoom — click to adjust (Ctrl+= / Ctrl+- / Ctrl+0, Ctrl+wheel)"));
}
