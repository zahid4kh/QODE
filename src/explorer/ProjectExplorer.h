#pragma once

#include "git/GitTypes.h"

#include <QWidget>
#include <functional>

class QFileSystemModel;
class QMenu;
class QLabel;
class QTreeView;
class ExplorerTree;
class QFrame;
class QTimer;
class QToolButton;
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
    // A small bar over the bottom of the tree: what just happened, with an optional action (Undo) and a list of details.
    struct Notice {
        QString text;
        bool error = false;
        QString actionLabel;
        std::function<void()> action;
        QList<QPair<QString, std::function<void()>>> details; // menu entries ("" text = separator)
        int timeoutMs = 14000;                                 // 0 = stays until dismissed
    };
    void showNotice(const Notice &notice);
    void hideNotice();
    void notifyMoved(const QString &from, const QString &to); // after the files were moved on disk
    void revealPaths(const QStringList &paths);               // select and scroll to them as soon as the model knows them
    void createFileIn(const QString &dir);
    void createFolderIn(const QString &dir);
    void copyToClipboard(const QStringList &paths); // as files, so they paste here and in file managers
    void pasteInto(const QString &dir);             // files or an image from the clipboard
    void copyInto(const QStringList &sources, const QString &dir); // dropped or pasted files

signals:
    void moveRequested(const QStringList &sources, const QString &targetDir); // dropped on a folder
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

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void onDoubleClicked(const QModelIndex &proxyIndex);
    void showContextMenu(const QPoint &pos);
    QString pathFor(const QModelIndex &proxyIndex) const;
    void renamePath(const QString &path);
    void deletePath(const QString &path);
    void addClipboardActions(QMenu *menu, const QString &path, const QString &dir);
    bool clipboardHasContent() const;
    void showError(const QString &title, const QString &text);
    void addGitActions(QMenu *menu, const QString &path);

    QString m_root;
    QLabel *m_title;
    QLabel *m_placeholder;
    ExplorerTree *m_tree;
    QFileSystemModel *m_fsModel;
    ProjectModel *m_proxy;
    GitRepository *m_git = nullptr;

    void layoutNotice();
    QFrame *m_notice = nullptr;
    QLabel *m_noticeText = nullptr;
    QToolButton *m_noticeAction = nullptr;
    QToolButton *m_noticeDetails = nullptr;
    QToolButton *m_noticeClose = nullptr;
    QTimer *m_noticeTimer = nullptr;
    Notice m_currentNotice;
    QStringList m_revealPaths;
    QTimer *m_revealTimer = nullptr;
    int m_revealTries = 0;
};
