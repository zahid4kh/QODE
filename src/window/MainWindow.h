#pragma once

#include "project/Project.h"

#include <QMainWindow>

class Document;
class EditorManager;
class ProjectManager;
class ProjectExplorer;
class QLabel;
class QSplitter;

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
    void about();
    void forwardToFocus(const char *slot);

    ProjectManager *m_projects;
    ProjectExplorer *m_explorer;
    EditorManager *m_editors;
    QSplitter *m_hsplit;

    QAction *m_newProjectAct, *m_openProjectAct, *m_closeProjectAct, *m_exitAct;
    QAction *m_newFileAct, *m_openFileAct, *m_saveAct, *m_saveAsAct, *m_saveAllAct, *m_closeFileAct;
    QAction *m_undoAct, *m_redoAct, *m_cutAct, *m_copyAct, *m_pasteAct, *m_selectAllAct, *m_findAct, *m_replaceAct;
    QAction *m_projNewFileAct, *m_projNewFolderAct, *m_openProjectFolderAct;
    QAction *m_explorerAct, *m_fullscreenAct, *m_wordWrapAct, *m_darkThemeAct, *m_lightThemeAct;
    QAction *m_nextTabAct, *m_prevTabAct, *m_aboutAct;

    QLabel *m_fileLabel, *m_langLabel, *m_encLabel, *m_eolLabel, *m_posLabel, *m_modeLabel;
};
