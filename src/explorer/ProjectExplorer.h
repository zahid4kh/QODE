#pragma once

#include "git/GitTypes.h"

#include <QWidget>

class QFileSystemModel;
class QMenu;
class QLabel;
class QTreeView;
class ProjectModel;
class GitRepository;

class ProjectExplorer : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectExplorer(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // empty => no project
    void setGitRepository(GitRepository *repo); // enables status colours/badges and Git context actions
    void setShowHidden(bool on); // dot files/folders (except .env*)
    QString projectRoot() const { return m_root; }

    // Directory that "New File"/"New Folder" should target by default.
    QString currentDirectory() const;

    void refresh();
    void createFileIn(const QString &dir);
    void createFolderIn(const QString &dir);

signals:
    void fileActivated(const QString &path);
    void fileCreated(const QString &path);
    void pathRenamed(const QString &oldPath, const QString &newPath);
    void pathDeleted(const QString &path);
    void closeProjectRequested();
    void contentsChanged(); // the file system model saw files appear/disappear/change
    // Git context-menu actions
    void gitStageRequested(const QStringList &paths);
    void gitUnstageRequested(const QStringList &paths);
    void gitDiscardRequested(const QStringList &paths);
    void gitDiffRequested(const QString &path, GitDiffMode mode);
    void gitIgnoreRequested(const QString &path);

private:
    void onDoubleClicked(const QModelIndex &proxyIndex);
    void showContextMenu(const QPoint &pos);
    QString pathFor(const QModelIndex &proxyIndex) const;
    void renamePath(const QString &path);
    void deletePath(const QString &path);
    void showError(const QString &title, const QString &text);
    void addGitActions(QMenu *menu, const QString &path);

    QString m_root;
    QLabel *m_title;
    QLabel *m_placeholder;
    QTreeView *m_tree;
    QFileSystemModel *m_fsModel;
    ProjectModel *m_proxy;
    GitRepository *m_git = nullptr;
};
