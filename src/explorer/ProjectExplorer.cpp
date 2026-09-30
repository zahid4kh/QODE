#include "ProjectExplorer.h"

#include "dialogs/NewFileDialog.h"
#include "filesystem/FileManager.h"
#include "project/ProjectModel.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QTreeView>
#include <QVBoxLayout>

ProjectExplorer::ProjectExplorer(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(tr("PROJECT"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));

    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    m_fsModel->setReadOnly(true);

    m_proxy = new ProjectModel(this);
    m_proxy->setSourceModel(m_fsModel);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->setAnimated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(14);
    for (int c = 1; c < m_fsModel->columnCount(); ++c)
        m_tree->hideColumn(c);
    m_tree->setVisible(false);

    m_placeholder = new QLabel(tr("No project open.\n\nUse File → New Project or\nFile → Open Project."), this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setObjectName(QStringLiteral("emptyText"));
    m_placeholder->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_title);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_placeholder, 1);

    connect(m_tree, &QTreeView::doubleClicked, this, &ProjectExplorer::onDoubleClicked);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &ProjectExplorer::showContextMenu);
}

void ProjectExplorer::setProjectRoot(const QString &root)
{
    m_root = root;
    const bool has = !root.isEmpty();
    m_tree->setVisible(has);
    m_placeholder->setVisible(!has);
    m_title->setText(has ? tr("PROJECT — %1").arg(QFileInfo(root).fileName().toUpper()) : tr("PROJECT"));

    if (!has) {
        m_proxy->setProjectRoot({});
        m_fsModel->setRootPath(QString());
        return;
    }
    const QString parentDir = QFileInfo(root).absolutePath();
    m_proxy->setProjectRoot(root);
    m_fsModel->setRootPath(parentDir);
    m_tree->setRootIndex(m_proxy->mapFromSource(m_fsModel->index(parentDir)));
    const QModelIndex rootIdx = m_proxy->mapFromSource(m_fsModel->index(root));
    m_tree->expand(rootIdx);
    m_tree->setCurrentIndex(rootIdx);
}

QString ProjectExplorer::pathFor(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid())
        return {};
    return m_fsModel->filePath(m_proxy->mapToSource(proxyIndex));
}

QString ProjectExplorer::currentDirectory() const
{
    const QString p = pathFor(m_tree->currentIndex());
    if (p.isEmpty())
        return m_root;
    const QFileInfo fi(p);
    return fi.isDir() ? p : fi.absolutePath();
}

void ProjectExplorer::refresh()
{
    if (m_root.isEmpty())
        return;
    // Re-rooting forces QFileSystemModel to rescan, which also picks up changes inotify missed.
    const QString parentDir = QFileInfo(m_root).absolutePath();
    m_fsModel->setRootPath(QString());
    m_fsModel->setRootPath(parentDir);
    m_tree->setRootIndex(m_proxy->mapFromSource(m_fsModel->index(parentDir)));
    m_tree->expand(m_proxy->mapFromSource(m_fsModel->index(m_root)));
}

void ProjectExplorer::onDoubleClicked(const QModelIndex &idx)
{
    const QString p = pathFor(idx);
    if (!p.isEmpty() && QFileInfo(p).isFile())
        emit fileActivated(p);
}

void ProjectExplorer::showError(const QString &title, const QString &text)
{
    QMessageBox::warning(this, title, text);
}

void ProjectExplorer::createFileIn(const QString &dir)
{
    if (dir.isEmpty())
        return;
    NewFileDialog dlg(NewFileDialog::Kind::File, dir, {}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString path = QDir(dir).filePath(dlg.name());
    QString err;
    if (!FileManager::createFile(path, &err)) {
        showError(tr("Unable to create file"), tr("Path:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
        return;
    }
    emit fileCreated(path);
}

void ProjectExplorer::createFolderIn(const QString &dir)
{
    if (dir.isEmpty())
        return;
    NewFileDialog dlg(NewFileDialog::Kind::Folder, dir, {}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString path = QDir(dir).filePath(dlg.name());
    QString err;
    if (!FileManager::createFolder(path, &err)) {
        showError(tr("Unable to create folder"), tr("Path:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
        return;
    }
    const QModelIndex parent = m_proxy->mapFromSource(m_fsModel->index(dir));
    m_tree->expand(parent);
}

void ProjectExplorer::renamePath(const QString &path)
{
    const QFileInfo fi(path);
    NewFileDialog dlg(NewFileDialog::Kind::Rename, fi.absolutePath(), fi.fileName(), this);
    if (dlg.exec() != QDialog::Accepted || dlg.name() == fi.fileName())
        return;
    const QString target = QDir(fi.absolutePath()).filePath(dlg.name());
    QString err;
    if (!FileManager::renamePath(path, target, &err)) {
        showError(tr("Unable to rename"), tr("Path:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
        return;
    }
    emit pathRenamed(path, target);
}

void ProjectExplorer::deletePath(const QString &path)
{
    const QFileInfo fi(path);
    const QString what = fi.isDir() ? tr("folder \"%1\" and everything in it").arg(fi.fileName())
                                    : tr("file \"%1\"").arg(fi.fileName());
    const auto answer = QMessageBox::warning(this, tr("Delete"), tr("Permanently delete %1?\n\nThis cannot be undone.").arg(what),
                                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    QString err;
    if (!FileManager::removePath(path, &err)) {
        showError(tr("Unable to delete"), tr("Path:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
        return;
    }
    emit pathDeleted(path);
}

void ProjectExplorer::showContextMenu(const QPoint &pos)
{
    if (m_root.isEmpty())
        return;
    QModelIndex idx = m_tree->indexAt(pos);
    const QString path = idx.isValid() ? pathFor(idx) : m_root;
    if (idx.isValid())
        m_tree->setCurrentIndex(idx);
    const QFileInfo fi(path);
    const bool isRoot = path == m_root;
    const bool isDir = fi.isDir();
    const QString dir = isDir ? path : fi.absolutePath();

    QMenu menu(this);
    if (isRoot) {
        menu.addAction(tr("New File"), this, [this, dir] { createFileIn(dir); });
        menu.addAction(tr("New Folder"), this, [this, dir] { createFolderIn(dir); });
        menu.addSeparator();
        menu.addAction(tr("Refresh"), this, &ProjectExplorer::refresh);
        menu.addAction(tr("Open in File Manager"), this, [path] { FileManager::revealInFileManager(path); });
        menu.addSeparator();
        menu.addAction(tr("Close Project"), this, &ProjectExplorer::closeProjectRequested);
    } else {
        if (isDir) {
            const bool expanded = m_tree->isExpanded(idx);
            menu.addAction(expanded ? tr("Collapse") : tr("Open"), this, [this, idx, expanded] { m_tree->setExpanded(idx, !expanded); });
            menu.addAction(tr("New File"), this, [this, dir] { createFileIn(dir); });
            menu.addAction(tr("New Folder"), this, [this, dir] { createFolderIn(dir); });
        } else {
            menu.addAction(tr("Open"), this, [this, path] { emit fileActivated(path); });
            menu.addAction(tr("Open in New Tab"), this, [this, path] { emit fileActivated(path); });
            menu.addAction(tr("New File"), this, [this, dir] { createFileIn(dir); });
        }
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, [this, path] { renamePath(path); });
        menu.addAction(tr("Delete"), this, [this, path] { deletePath(path); });
        menu.addSeparator();
        menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
        menu.addAction(tr("Reveal in File Manager"), this, [path] { FileManager::revealInFileManager(path); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
