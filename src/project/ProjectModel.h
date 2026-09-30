#pragma once

#include <QSortFilterProxyModel>

// Presents a QFileSystemModel so that the project root is the single top-level
// node (rather than the contents of its parent directory) and hides VCS internals.
class ProjectModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit ProjectModel(QObject *parent = nullptr);

    void setProjectRoot(const QString &root);
    QString projectRoot() const { return m_root; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_root;
};
