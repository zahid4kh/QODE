#include "ProjectModel.h"

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
