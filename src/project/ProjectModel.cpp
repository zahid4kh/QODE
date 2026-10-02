#include "ProjectModel.h"

#include "explorer/FileIcons.h"
#include "git/GitRepository.h"

#include <QCollator>
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

void ProjectModel::setShowHidden(bool on)
{
    if (m_showHidden == on)
        return;
    m_showHidden = on;
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
    const QString name = fs->fileName(idx);
    if (name == QLatin1String(".git"))
        return false;
    if (!m_showHidden && name.startsWith(QLatin1Char('.')) && !name.startsWith(QLatin1String(".env")))
        return false;
    return true;
}

bool ProjectModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    auto *fs = qobject_cast<QFileSystemModel *>(sourceModel());
    if (!fs)
        return QSortFilterProxyModel::lessThan(left, right);
    auto group = [fs](const QModelIndex &i) {
        const bool dot = fs->fileName(i).startsWith(QLatin1Char('.'));
        const bool dir = fs->isDir(i);
        return dir ? (dot ? 1 : 0) : (dot ? 2 : 3);
    };
    const int gl = group(left), gr = group(right);
    if (gl != gr)
        return gl < gr;
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator.compare(fs->fileName(left), fs->fileName(right)) < 0;
}
