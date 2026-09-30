#pragma once

#include "project/Project.h"

#include <QMainWindow>

class ProjectManager;
class ProjectExplorer;
class QSplitter;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openInitialPaths(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createMenus();
    void restoreSettings();

    void newProject();
    void openProject();
    void openProjectPath(const QString &path);
    bool closeProject();
    void onProjectOpened(const Project &p);
    void onProjectClosed();
    void updateTitle();
    void about();

    ProjectManager *m_projects;
    ProjectExplorer *m_explorer;
    QSplitter *m_hsplit;

    QAction *m_newProjectAct, *m_openProjectAct, *m_closeProjectAct, *m_exitAct;
    QAction *m_newFileAct, *m_newFolderAct, *m_openProjectFolderAct;
    QAction *m_explorerAct, *m_fullscreenAct, *m_aboutAct;
};
