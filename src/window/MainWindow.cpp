#include "MainWindow.h"

#include "Island.h"
#include "dialogs/NewProjectDialog.h"
#include "dialogs/RunConfigDialog.h"
#include "git/BranchButton.h"
#include "git/BranchPopup.h"
#include "git/DiffDialog.h"
#include "git/GitDiff.h"
#include "git/GitPanel.h"
#include "git/GitRepository.h"
#include "palette/PalettePopup.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/FileIcons.h"
#include "explorer/ProjectExplorer.h"
#include "filesystem/FileManager.h"
#include "media/MediaPanel.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "project/PythonEnv.h"
#include "search/ProjectSearch.h"
#include "search/SearchPanel.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "terminal/Terminal.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
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
    m_editors = new EditorManager(this);

    // Left side: Explorer / Source Control switcher
    m_side = new QWidget(this);
    m_sideTabs = new QTabBar(m_side);
    m_sideTabs->addTab(tr("Explorer"));
    m_sideTabs->addTab(tr("Source Control"));
    m_sideTabs->addTab(tr("Search"));
    m_sideTabs->setObjectName(QStringLiteral("sideTabs"));
    m_sideTabs->setExpanding(true);
    m_sideTabs->setDrawBase(false);
    m_sideTabs->setUsesScrollButtons(false);
    m_sideTabs->setElideMode(Qt::ElideNone);
    m_sideStack = new QStackedWidget(m_side);
    m_sideStack->addWidget(m_explorer);
    m_sideStack->addWidget(m_gitPanel);
    m_sideStack->addWidget(m_searchPanel);
    auto *sideLayout = new QVBoxLayout(m_side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    sideLayout->addWidget(m_sideTabs);
    sideLayout->addWidget(m_sideStack, 1);
    connect(m_sideTabs, &QTabBar::currentChanged, m_sideStack, &QStackedWidget::setCurrentIndex);

    m_terminal = new Terminal(this);
    // Python projects: activate the project's virtualenv (whatever its folder is called) in fresh shells.
    m_terminal->setStartupCommandProvider([this](const QString &shell) {
        if (!m_projects->hasProject() || !SettingsManager::instance().autoActivateVenv())
            return QString();
        return PythonEnv::activationCommand(m_projects->project().root, shell);
    });

    m_vsplit = new QSplitter(Qt::Vertical, this);
    m_vsplit->setChildrenCollapsible(false);
    m_vsplit->setHandleWidth(8);
    m_vsplit->addWidget(new Island(m_editors));
    m_vsplit->addWidget(new Island(m_terminal));
    m_vsplit->setStretchFactor(0, 1);
    m_vsplit->setStretchFactor(1, 0);
    m_vsplit->widget(1)->hide(); // hidden until Ctrl+J

    m_hsplit = new QSplitter(Qt::Horizontal, this);
    m_hsplit->setChildrenCollapsible(false);
    m_hsplit->setHandleWidth(8);
    m_hsplit->setContentsMargins(8, 2, 8, 0);
    m_hsplit->addWidget(new Island(m_side));
    m_hsplit->addWidget(m_vsplit);
    m_media = new MediaPanel(this);
    m_hsplit->addWidget(new Island(m_media));
    m_hsplit->setStretchFactor(0, 0);
    m_hsplit->setStretchFactor(1, 1);
    m_hsplit->setStretchFactor(2, 0);
    m_hsplit->widget(2)->hide(); // shown when an image or video is opened
    setCentralWidget(m_hsplit);

    createActions();
    createMenus();
    createToolBar();
    createStatusBar();

    connect(m_projects, &ProjectManager::projectOpened, this, &MainWindow::onProjectOpened);
    connect(m_projects, &ProjectManager::projectClosed, this, &MainWindow::onProjectClosed);
    connect(m_explorer, &ProjectExplorer::closeProjectRequested, this, &MainWindow::closeProject);
    connect(m_explorer, &ProjectExplorer::fileActivated, m_editors, &EditorManager::openFile);
    connect(m_explorer, &ProjectExplorer::fileCreated, m_editors, &EditorManager::openFile);
    connect(m_explorer, &ProjectExplorer::pathRenamed, m_editors, &EditorManager::pathRenamed);
    connect(m_explorer, &ProjectExplorer::pathDeleted, m_editors, &EditorManager::closeDocumentsUnder);
    connect(m_explorer, &ProjectExplorer::contentsChanged, m_projectFiles, &ProjectFiles::invalidate);
    connect(m_editors, &EditorManager::documentPathChanged, m_projectFiles, &ProjectFiles::invalidate);

    connect(m_editors, &EditorManager::mediaRequested, this, &MainWindow::showMedia);
    connect(m_media, &MediaPanel::closeRequested, this, &MainWindow::hideMedia);
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

    connect(m_terminal, &Terminal::hideRequested, this, &MainWindow::toggleTerminal);
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

    m_explorerAct = make(tr("Project Explorer"), QKeySequence(C | K::Key_B));
    m_explorerAct->setCheckable(true);
    m_explorerAct->setChecked(true);
    m_terminalAct = make(tr("Terminal"), QKeySequence(C | K::Key_J), QStringLiteral(":/new-icons/terminal.svg"));
    m_terminalAct->setCheckable(true);
    m_venvAct = make(tr("Auto-Activate Python venv"));
    m_venvAct->setCheckable(true);
    m_venvAct->setChecked(SettingsManager::instance().autoActivateVenv());
    connect(m_venvAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setAutoActivateVenv(on); });
    m_runAct = make(tr("Run File"), QKeySequence(K::Key_F5));
    m_runAct->setToolTip(tr("Run this file (F5)"));
    m_runConfigAct = make(tr("Run Configuration…"), {}, QStringLiteral(":/new-icons/cog.svg"));
    m_runConfigAct->setToolTip(tr("Set the command that runs this type of file"));
    {
        // Green outlined play button; recoloured with the theme since green must differ on light/dark.
        auto paint = [this] {
            const bool dark = SettingsManager::instance().theme() != QLatin1String("light");
            m_runAct->setIcon(Icons::tinted(QStringLiteral(":/new-icons/play.svg"),
                                            QColor(dark ? QStringLiteral("#4ec969") : QStringLiteral("#1a8f3c"))));
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
    m_indentGuidesAct = make(tr("Indent Guides"));
    m_indentGuidesAct->setCheckable(true);
    m_indentGuidesAct->setChecked(SettingsManager::instance().indentGuides());
    m_darkThemeAct = make(tr("Dark"));
    m_lightThemeAct = make(tr("Light"));
    m_darkThemeAct->setCheckable(true);
    m_lightThemeAct->setCheckable(true);
    auto *themeGroup = new QActionGroup(this);
    themeGroup->addAction(m_darkThemeAct);
    themeGroup->addAction(m_lightThemeAct);
    (SettingsManager::instance().theme() == QLatin1String("light") ? m_lightThemeAct : m_darkThemeAct)->setChecked(true);

    m_nextTabAct = make(tr("Next Tab"), QKeySequence(C | K::Key_Tab));
    m_prevTabAct = make(tr("Previous Tab"), QKeySequence(C | S | K::Key_Backtab));
    m_aboutAct = make(tr("About QODE"));
    m_paletteAct = make(tr("Command Palette…"), QKeySequence(C | S | K::Key_P), QStringLiteral(":/new-icons/search.svg"));
    m_quickOpenAct = make(tr("Go to File…"), QKeySequence(C | K::Key_P), QStringLiteral(":/new-icons/file-input.svg"));
    m_gotoLineAct = make(tr("Go to Line…"), QKeySequence(C | K::Key_G));
    m_matchBracketAct = make(tr("Go to Matching Bracket"), QKeySequence(C | S | K::Key_Backslash));
    auto &cfg = SettingsManager::instance();
    m_autoSaveOffAct = make(tr("Off"));
    m_autoSaveDelayAct = make(tr("After Delay"));
    m_autoSaveFocusAct = make(tr("On Focus Change"));
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
    connect(m_trimAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setTrimTrailingWhitespace(on); });
    connect(m_finalNewlineAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setInsertFinalNewline(on); });
    connect(m_formatOnSaveAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setFormatOnSave(on); });
    connect(m_formatAct, &QAction::triggered, m_editors, &EditorManager::formatCurrent);
    connect(m_editors, &EditorManager::statusMessage, this, [this](const QString &t) { statusBar()->showMessage(t, 6000); });

    m_foldAct = make(tr("Fold"), QKeySequence(C | S | K::Key_BracketLeft));
    m_unfoldAct = make(tr("Unfold"), QKeySequence(C | S | K::Key_BracketRight));
    m_foldAllAct = make(tr("Fold All"), QKeySequence(QStringLiteral("Ctrl+K, Ctrl+0")));
    m_unfoldAllAct = make(tr("Unfold All"), QKeySequence(QStringLiteral("Ctrl+K, Ctrl+J")));
    m_searchAct = make(tr("Find in Files"), QKeySequence(C | S | K::Key_F), QStringLiteral(":/new-icons/search.svg"));

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

    connect(m_explorerAct, &QAction::toggled, this, [this](bool on) { m_side->parentWidget()->setVisible(on); });
    connect(m_terminalAct, &QAction::triggered, this, &MainWindow::toggleTerminal);
    connect(m_runAct, &QAction::triggered, this, &MainWindow::runCurrentFile);
    connect(m_runConfigAct, &QAction::triggered, this, &MainWindow::configureRun);
    connect(m_fullscreenAct, &QAction::triggered, this, [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
    connect(m_wordWrapAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setWordWrap(on); });
    connect(m_breadcrumbsAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setShowBreadcrumbs(on); });
    connect(m_minimapAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setShowMinimap(on); });
    connect(m_indentGuidesAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setIndentGuides(on); });
    connect(m_darkThemeAct, &QAction::triggered, this, [] { SettingsManager::instance().setTheme(QStringLiteral("dark")); });
    connect(m_lightThemeAct, &QAction::triggered, this, [] { SettingsManager::instance().setTheme(QStringLiteral("light")); });
    connect(m_nextTabAct, &QAction::triggered, m_editors, &EditorManager::nextTab);
    connect(m_prevTabAct, &QAction::triggered, m_editors, &EditorManager::previousTab);
    connect(m_aboutAct, &QAction::triggered, this, &MainWindow::about);
    connect(m_paletteAct, &QAction::triggered, this, &MainWindow::showCommandPalette);
    connect(m_quickOpenAct, &QAction::triggered, this, [this] { showQuickOpen(); });
    connect(m_gotoLineAct, &QAction::triggered, this, [this] { showQuickOpen(QStringLiteral(":")); });
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
    view->addAction(m_venvAct);
    view->addSeparator();
    view->addAction(m_runAct);
    view->addAction(m_runConfigAct);
    view->addSeparator();
    view->addAction(m_wordWrapAct);
    view->addAction(m_indentGuidesAct);
    view->addAction(m_minimapAct);
    view->addAction(m_breadcrumbsAct);
    QMenu *theme = view->addMenu(tr("Theme"));
    theme->addAction(m_darkThemeAct);
    theme->addAction(m_lightThemeAct);
    view->addSeparator();
    view->addAction(m_fullscreenAct);

    QMenu *project = menuBar()->addMenu(tr("&Project"));
    project->addAction(m_projNewFileAct);
    project->addAction(m_projNewFolderAct);
    project->addSeparator();
    project->addAction(m_openProjectFolderAct);
    project->addAction(m_closeProjectAct);

    createGitMenu(menuBar()->addMenu(tr("&Git")));

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_aboutAct);
}

void MainWindow::createToolBar()
{
    QToolBar *tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(16, 16));
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
    tb->addAction(m_runAct);
    tb->addAction(m_runConfigAct);
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
    m_langLabel = mk(90);
    m_encLabel = mk(70);
    m_eolLabel = mk(40);
    m_posLabel = mk(110);
    m_modeLabel = mk(36);
    statusBar()->setSizeGripEnabled(false);
}

void MainWindow::restoreSettings()
{
    auto &s = SettingsManager::instance();
    resize(1200, 760);
    if (!s.windowGeometry().isEmpty())
        restoreGeometry(s.windowGeometry());
    m_hsplit->setSizes({s.explorerWidth(), qMax(400, width() - s.explorerWidth())});
    m_terminalHeight = s.terminalHeight();
}

// --- Startup / session ------------------------------------------------------

void MainWindow::openInitialPaths(const QStringList &paths)
{
    if (paths.isEmpty()) {
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
}

void MainWindow::restoreSession()
{
    auto &s = SettingsManager::instance();
    const QString project = s.lastProject();
    if (!project.isEmpty() && QFileInfo(project).isDir()) {
        QString err;
        m_projects->openProject(project, &err); // silently skip if it can't be opened
    }
    for (const QString &f : s.openFiles())
        if (QFileInfo(f).isFile())
            m_editors->openFile(f);
    const QString active = s.activeFile();
    if (!active.isEmpty() && QFileInfo(active).isFile())
        m_editors->openFile(active);
}

// --- Projects -----------------------------------------------------------------

void MainWindow::newProject()
{
    NewProjectDialog dlg(SettingsManager::instance().lastDirectory(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (m_projects->hasProject() && !closeProject())
        return;
    QString err;
    SettingsManager::instance().setLastDirectory(dlg.location());
    if (!m_projects->createProject(dlg.projectName(), dlg.location(), &err))
        QMessageBox::warning(this, tr("Unable to create project"), err);
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
    if (m_projects->hasProject() && !closeProject())
        return;
    QString err;
    if (!m_projects->openProject(path, &err))
        QMessageBox::warning(this, tr("Unable to open project"), tr("Unable to open project.\n\nPath:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
    else
        SettingsManager::instance().setLastDirectory(QFileInfo(path).absolutePath());
}

bool MainWindow::closeProject()
{
    if (!m_projects->hasProject())
        return true;
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
    SettingsManager::instance().addRecentProject(p.root);
    m_explorer->setProjectRoot(p.root);
    m_projectFiles->setRoot(p.root);
    m_editors->setProjectRoot(p.root);
    m_searchPanel->setProjectRoot(p.root);
    m_media->setProjectRoot(p.root);
    m_git->setWorkDirectory(p.root);
    // The shell always starts in the project root.
    m_terminal->setWorkingDirectory(p.root);
    if (m_terminal->isRunning())
        m_terminal->restart();
    else if (m_terminal->isVisible())
        m_terminal->ensureStarted();
    updateTitle();
    updateActions();
}

void MainWindow::onProjectClosed()
{
    m_explorer->setProjectRoot({});
    m_projectFiles->setRoot({});
    m_editors->setProjectRoot({});
    m_searchPanel->setProjectRoot({});
    hideMedia();
    m_media->setProjectRoot({});
    m_git->setWorkDirectory({});
    m_terminal->setWorkingDirectory(QDir::homePath());
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
    m_runAct->setEnabled(!currentFilePath().isEmpty() || (hasDoc && m_editors->currentDocument()->isUntitled()));
    m_runConfigAct->setEnabled(!currentFilePath().isEmpty());

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

void MainWindow::showMedia(const QString &path)
{
    m_media->openMedia(path);
    QWidget *island = m_media->parentWidget();
    if (island->isVisible())
        return;
    island->show();
    QList<int> sizes = m_hsplit->sizes();
    const int w = qBound(280, m_mediaWidth, qMax(280, m_hsplit->width() / 2));
    sizes[2] = w;
    sizes[1] = qMax(200, sizes[1] - w);
    m_hsplit->setSizes(sizes);
}

void MainWindow::hideMedia()
{
    QWidget *island = m_media->parentWidget();
    if (island->isVisible())
        m_mediaWidth = island->width();
    m_media->clear();
    island->hide();
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

void MainWindow::configureRun()
{
    const QString path = currentFilePath();
    if (path.isEmpty())
        return;
    const QString key = RunConfigDialog::keyFor(path);
    SettingsManager &s = SettingsManager::instance();
    RunConfigDialog dlg(path, s.runCommand(key), this);
    if (dlg.exec() == QDialog::Accepted)
        s.setRunCommand(key, dlg.command());
}

void MainWindow::runCurrentFile()
{
    Document *doc = m_editors->currentDocument();
    if (!doc)
        return;
    if (doc->isUntitled() && !m_editors->saveCurrent())
        return;
    const QString path = currentFilePath();
    if (path.isEmpty())
        return;
    SettingsManager &s = SettingsManager::instance();
    const QString key = RunConfigDialog::keyFor(path);
    if (s.runCommand(key).isEmpty()) {
        // First run of this file type: ask for the command, then carry on.
        configureRun();
        if (s.runCommand(key).isEmpty())
            return;
    }
    if (doc->isModified() && !m_editors->saveCurrent())
        return;
    showTerminal();
    m_terminal->runCommand(RunConfigDialog::expand(s.runCommand(key), path, m_projects->project().root));
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

void MainWindow::showSearch()
{
    if (!m_explorerAct->isChecked())
        m_explorerAct->setChecked(true);
    m_sideTabs->setCurrentIndex(2);
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
        s.setExplorerWidth(m_hsplit->sizes().value(0));
    if (m_terminal->isVisible())
        m_terminalHeight = m_vsplit->sizes().value(1, m_terminalHeight);
    s.setTerminalHeight(m_terminalHeight);
    m_terminal->stop();
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
            a->setIcon(QIcon(icon));
        return a;
    };
    m_scmAct = make(tr("Source Control"), QKeySequence(C | S | K::Key_G), QStringLiteral(":/new-icons/git-branch.svg"));
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
        m_gitStatusWidget->show();
    } else {
        m_gitStatusWidget->hide();
    }
    const int n = m_git->isRepo() ? m_git->changes().size() : 0;
    m_sideTabs->setTabText(1, n > 0 ? tr("Source Control (%1)").arg(n) : tr("Source Control"));

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
        return;
    }
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
    m_sideTabs->setCurrentIndex(1);
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
