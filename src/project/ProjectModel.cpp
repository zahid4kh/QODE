#include "ProjectModel.h"

#include "explorer/FileIcons.h"
#include "git/GitRepository.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>

ProjectModel::ProjectModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
}

void ProjectModel::setProjectRoot(const QString &root)
{
    m_root = root;
    invalidateFilter();
}

QVariant ProjectModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DecorationRole && index.column() == 0) {
        // Our own icons instead of the (system dependent) file icon provider; also safe to build here
        // because the model is queried on the GUI thread.
        if (auto *fs = qobject_cast<QFileSystemModel *>(sourceModel())) {
            const QModelIndex src = mapToSource(index);
            return fs->isDir(src) ? FileIcons::folder() : FileIcons::forFile(fs->fileName(src));
        }
    }
    if (role == PathRole || role == Qt::ToolTipRole) {
        if (auto *fs = qobject_cast<QFileSystemModel *>(sourceModel())) {
            const QString path = fs->filePath(mapToSource(index));
            if (role == PathRole)
                return path;
            QString tip = QDir::toNativeSeparators(path);
            if (m_git) {
                const GitPathState st = m_git->stateOf(path);
                if (st.kind != GitKind::None)
                    tip += QLatin1Char('\n') + gitDescribe(st);
            }
            return tip;
        }
    }
    return QSortFilterProxyModel::data(index, role);
}

bool ProjectModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    auto *fs = qobject_cast<QFileSystemModel *>(sourceModel());
    if (!fs || m_root.isEmpty())
        return true;
    const QModelIndex idx = fs->index(sourceRow, 0, sourceParent);
    const QString path = fs->filePath(idx);
    // Top level (children of the project's parent directory): only the project itself.
    if (fs->filePath(sourceParent) == QFileInfo(m_root).absolutePath() && !path.isEmpty())
        return path == m_root;
    return fs->fileName(idx) != QLatin1String(".git");
}
