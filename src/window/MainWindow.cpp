#include "MainWindow.h"

#include "dialogs/NewProjectDialog.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/ProjectExplorer.h"
#include "filesystem/FileManager.h"
#include "project/ProjectManager.h"
#include "settings/SettingsManager.h"
#include "terminal/Terminal.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setAcceptDrops(true);
    m_projects = new ProjectManager(this);
    m_explorer = new ProjectExplorer(this);
    m_editors = new EditorManager(this);

    m_terminal = new Terminal(this);

    m_vsplit = new QSplitter(Qt::Vertical, this);
    m_vsplit->setChildrenCollapsible(false);
    m_vsplit->addWidget(m_editors);
    m_vsplit->addWidget(m_terminal);
    m_vsplit->setStretchFactor(0, 1);
    m_vsplit->setStretchFactor(1, 0);
    m_terminal->hide(); // hidden until Ctrl+J

    m_hsplit = new QSplitter(Qt::Horizontal, this);
    m_hsplit->setChildrenCollapsible(false);
    m_hsplit->addWidget(m_explorer);
    m_hsplit->addWidget(m_vsplit);
    m_hsplit->setStretchFactor(0, 0);
    m_hsplit->setStretchFactor(1, 1);
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

    connect(m_terminal, &Terminal::hideRequested, this, &MainWindow::toggleTerminal);
    connect(m_editors, &EditorManager::currentChanged, this, [this] { updateStatus(); updateActions(); updateTitle(); });
    connect(m_editors, &EditorManager::documentStateChanged, this, [this] { updateStatus(); updateActions(); updateTitle(); });
    connect(m_editors, &EditorManager::cursorInfoChanged, this, &MainWindow::updateStatus);

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
            a->setIcon(QIcon(icon));
        return a;
    };
    using K = Qt::Key;
    const auto C = Qt::CTRL;
    const auto S = Qt::SHIFT;
    const auto A = Qt::ALT;

    m_newProjectAct = make(tr("New Project…"), QKeySequence(C | S | K::Key_N));
    m_openProjectAct = make(tr("Open Project…"), QKeySequence(C | S | K::Key_O), QStringLiteral(":/icons/folder.svg"));
    m_closeProjectAct = make(tr("Close Project"));
    m_exitAct = make(tr("Exit"), QKeySequence(C | K::Key_Q));

    m_newFileAct = make(tr("New File…"), QKeySequence(C | K::Key_N), QStringLiteral(":/icons/new-file.svg"));
    m_openFileAct = make(tr("Open File…"), QKeySequence(C | K::Key_O), QStringLiteral(":/icons/open.svg"));
    m_saveAct = make(tr("Save"), QKeySequence(C | K::Key_S), QStringLiteral(":/icons/save.svg"));
    m_saveAsAct = make(tr("Save As…"), QKeySequence(C | S | K::Key_S));
    m_saveAllAct = make(tr("Save All"), QKeySequence(C | S | A | K::Key_S));
    m_closeFileAct = make(tr("Close File"), QKeySequence(C | K::Key_W));

    m_undoAct = make(tr("Undo"), QKeySequence(C | K::Key_Z));
    m_redoAct = make(tr("Redo"), QKeySequence(C | S | K::Key_Z));
    m_cutAct = make(tr("Cut"), QKeySequence(C | K::Key_X));
    m_copyAct = make(tr("Copy"), QKeySequence(C | K::Key_C));
    m_pasteAct = make(tr("Paste"), QKeySequence(C | K::Key_V));
    m_selectAllAct = make(tr("Select All"), QKeySequence(C | K::Key_A));
    m_findAct = make(tr("Find"), QKeySequence(C | K::Key_F), QStringLiteral(":/icons/find.svg"));
    m_replaceAct = make(tr("Replace"), QKeySequence(C | K::Key_H));

    m_projNewFileAct = make(tr("New File…"), {}, QStringLiteral(":/icons/new-file.svg"));
    m_projNewFolderAct = make(tr("New Folder…"), {}, QStringLiteral(":/icons/new-folder.svg"));
    m_openProjectFolderAct = make(tr("Open Project Folder…"));

    m_explorerAct = make(tr("Project Explorer"), QKeySequence(C | K::Key_B));
    m_explorerAct->setCheckable(true);
    m_explorerAct->setChecked(true);
    m_terminalAct = make(tr("Terminal"), QKeySequence(C | K::Key_J), QStringLiteral(":/icons/terminal.svg"));
    m_terminalAct->setCheckable(true);
    m_fullscreenAct = make(tr("Toggle Fullscreen"), QKeySequence(K::Key_F11));
    m_wordWrapAct = make(tr("Word Wrap"), QKeySequence(A | K::Key_Z));
    m_wordWrapAct->setCheckable(true);
    m_wordWrapAct->setChecked(SettingsManager::instance().wordWrap());
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

    connect(m_explorerAct, &QAction::toggled, m_explorer, &QWidget::setVisible);
    connect(m_terminalAct, &QAction::triggered, this, &MainWindow::toggleTerminal);
    connect(m_fullscreenAct, &QAction::triggered, this, [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
    connect(m_wordWrapAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setWordWrap(on); });
    connect(m_darkThemeAct, &QAction::triggered, this, [] { SettingsManager::instance().setTheme(QStringLiteral("dark")); });
    connect(m_lightThemeAct, &QAction::triggered, this, [] { SettingsManager::instance().setTheme(QStringLiteral("light")); });
    connect(m_nextTabAct, &QAction::triggered, m_editors, &EditorManager::nextTab);
    connect(m_prevTabAct, &QAction::triggered, m_editors, &EditorManager::previousTab);
    connect(m_aboutAct, &QAction::triggered, this, &MainWindow::about);

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
    file->addSeparator();
    file->addAction(m_newFileAct);
    file->addAction(m_openFileAct);
    file->addSeparator();
    file->addAction(m_saveAct);
    file->addAction(m_saveAsAct);
    file->addAction(m_saveAllAct);
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
    edit->addAction(m_findAct);
    edit->addAction(m_replaceAct);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_explorerAct);
    view->addAction(m_terminalAct);
    view->addSeparator();
    view->addAction(m_wordWrapAct);
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
    tb->addAction(m_newFileAct);
    tb->addAction(m_projNewFolderAct);
    tb->addSeparator();
    tb->addAction(m_saveAct);
    tb->addAction(m_findAct);
    tb->addSeparator();
    tb->addAction(m_terminalAct);
}

void MainWindow::createStatusBar()
{
    auto mk = [this](int minWidth) {
        auto *l = new QLabel(this);
        l->setMinimumWidth(minWidth);
        statusBar()->addPermanentWidget(l);
        return l;
    };
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

void MainWindow::onProjectOpened(const Project &p)
{
    m_explorer->setProjectRoot(p.root);
    // The shell always starts in the project root; nothing is executed automatically.
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
    m_replaceAct->setEnabled(hasDoc);
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

void MainWindow::toggleTerminal()
{
    const bool show = !m_terminal->isVisible();
    if (show) {
        m_terminal->show();
        const int total = m_vsplit->height();
        const int h = qBound(80, m_terminalHeight, qMax(80, total - 100));
        m_vsplit->setSizes({total - h, h});
        m_terminal->ensureStarted();
        m_terminal->focusTerminal();
    } else {
        // Remember the height for the rest of the session.
        m_terminalHeight = m_vsplit->sizes().value(1, m_terminalHeight);
        m_terminal->hide();
        m_editors->focusEditor();
    }
    m_terminalAct->setChecked(show);
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
    if (m_explorer->isVisible())
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
