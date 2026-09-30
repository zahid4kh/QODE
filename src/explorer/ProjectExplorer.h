#pragma once

#include <QWidget>

class QFileSystemModel;
class QLabel;
class QTreeView;
class ProjectModel;

class ProjectExplorer : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectExplorer(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // empty => no project
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

private:
    void onDoubleClicked(const QModelIndex &proxyIndex);
    void showContextMenu(const QPoint &pos);
    QString pathFor(const QModelIndex &proxyIndex) const;
    void renamePath(const QString &path);
    void deletePath(const QString &path);
    void showError(const QString &title, const QString &text);

    QString m_root;
    QLabel *m_title;
    QLabel *m_placeholder;
    QTreeView *m_tree;
    QFileSystemModel *m_fsModel;
    ProjectModel *m_proxy;
};
