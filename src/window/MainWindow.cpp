#include "MainWindow.h"

#include "dialogs/NewProjectDialog.h"
#include "explorer/ProjectExplorer.h"
#include "filesystem/FileManager.h"
#include "project/ProjectManager.h"
#include "settings/SettingsManager.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_projects = new ProjectManager(this);
    m_explorer = new ProjectExplorer(this);

    m_hsplit = new QSplitter(Qt::Horizontal, this);
    m_hsplit->setChildrenCollapsible(false);
    m_hsplit->addWidget(m_explorer);
    m_hsplit->setStretchFactor(0, 0);
    setCentralWidget(m_hsplit);

    createActions();
    createMenus();
    statusBar();

    connect(m_projects, &ProjectManager::projectOpened, this, &MainWindow::onProjectOpened);
    connect(m_projects, &ProjectManager::projectClosed, this, &MainWindow::onProjectClosed);
    connect(m_explorer, &ProjectExplorer::closeProjectRequested, this, &MainWindow::closeProject);

    restoreSettings();
    onProjectClosed();
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
    m_newProjectAct = make(tr("New Project…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    m_openProjectAct = make(tr("Open Project…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    m_closeProjectAct = make(tr("Close Project"));
    m_exitAct = make(tr("Exit"), QKeySequence(Qt::CTRL | Qt::Key_Q));
    m_newFileAct = make(tr("New File…"));
    m_newFolderAct = make(tr("New Folder…"));
    m_openProjectFolderAct = make(tr("Open Project Folder…"));
    m_explorerAct = make(tr("Project Explorer"), QKeySequence(Qt::CTRL | Qt::Key_B));
    m_explorerAct->setCheckable(true);
    m_explorerAct->setChecked(true);
    m_fullscreenAct = make(tr("Toggle Fullscreen"), QKeySequence(Qt::Key_F11));
    m_aboutAct = make(tr("About QODE"));

    connect(m_newProjectAct, &QAction::triggered, this, &MainWindow::newProject);
    connect(m_openProjectAct, &QAction::triggered, this, &MainWindow::openProject);
    connect(m_openProjectFolderAct, &QAction::triggered, this, &MainWindow::openProject);
    connect(m_closeProjectAct, &QAction::triggered, this, &MainWindow::closeProject);
    connect(m_exitAct, &QAction::triggered, this, &QWidget::close);
    connect(m_newFileAct, &QAction::triggered, this, [this] { m_explorer->createFileIn(m_explorer->currentDirectory()); });
    connect(m_newFolderAct, &QAction::triggered, this, [this] { m_explorer->createFolderIn(m_explorer->currentDirectory()); });
    connect(m_explorerAct, &QAction::toggled, m_explorer, &QWidget::setVisible);
    connect(m_fullscreenAct, &QAction::triggered, this, [this] {
        setWindowState(windowState() ^ Qt::WindowFullScreen);
    });
    connect(m_aboutAct, &QAction::triggered, this, &MainWindow::about);
}

void MainWindow::createMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_newProjectAct);
    file->addAction(m_openProjectAct);
    file->addSeparator();
    file->addAction(m_exitAct);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_explorerAct);
    view->addAction(m_fullscreenAct);

    QMenu *project = menuBar()->addMenu(tr("&Project"));
    project->addAction(m_newFileAct);
    project->addAction(m_newFolderAct);
    project->addAction(m_openProjectFolderAct);
    project->addAction(m_closeProjectAct);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_aboutAct);
}

void MainWindow::restoreSettings()
{
    auto &s = SettingsManager::instance();
    resize(1200, 760);
    if (!s.windowGeometry().isEmpty())
        restoreGeometry(s.windowGeometry());
    m_hsplit->setSizes({s.explorerWidth(), 1000});
}

void MainWindow::openInitialPaths(const QStringList &paths)
{
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir())
            openProjectPath(fi.absoluteFilePath());
    }
}

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
        QMessageBox::warning(this, tr("Unable to open project"), tr("Path:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
    else
        SettingsManager::instance().setLastDirectory(QFileInfo(path).absolutePath());
}

bool MainWindow::closeProject()
{
    if (!m_projects->hasProject())
        return true;
    m_projects->closeProject();
    return true;
}

void MainWindow::onProjectOpened(const Project &p)
{
    m_explorer->setProjectRoot(p.root);
    m_closeProjectAct->setEnabled(true);
    m_newFileAct->setEnabled(true);
    m_newFolderAct->setEnabled(true);
    SettingsManager::instance().setLastProject(p.root);
    updateTitle();
}

void MainWindow::onProjectClosed()
{
    m_explorer->setProjectRoot({});
    m_closeProjectAct->setEnabled(false);
    m_newFileAct->setEnabled(false);
    m_newFolderAct->setEnabled(false);
    updateTitle();
}

void MainWindow::updateTitle()
{
    setWindowTitle(m_projects->hasProject() ? tr("%1 — QODE").arg(m_projects->project().name) : tr("QODE"));
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About QODE"),
                       tr("<h3>QODE %1</h3><p>A native, lightweight C++/Qt code editor built around "
                          "projects, files, code and an integrated terminal.</p>")
                           .arg(QCoreApplication::applicationVersion()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    auto &s = SettingsManager::instance();
    s.setWindowGeometry(saveGeometry());
    if (m_explorer->isVisible())
        s.setExplorerWidth(m_hsplit->sizes().value(0));
    event->accept();
}
