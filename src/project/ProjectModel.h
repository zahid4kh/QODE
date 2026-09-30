#pragma once

#include <QSortFilterProxyModel>

class GitRepository;

// Presents a QFileSystemModel so that the project root is the single top-level
// node (rather than the contents of its parent directory) and hides VCS internals.
class ProjectModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1 }; // absolute file path

    explicit ProjectModel(QObject *parent = nullptr);

    void setProjectRoot(const QString &root);
    QString projectRoot() const { return m_root; }
    void setGitRepository(GitRepository *repo) { m_git = repo; } // adds git status to tooltips

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_root;
    GitRepository *m_git = nullptr;
};
