#include "ReferencesDialog.h"

#include "explorer/FileIcons.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
enum Role { RolePath = Qt::UserRole + 1, RoleLine, RoleColumn };
}

ReferencesDialog::ReferencesDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("References"));
    setModal(false);
    resize(720, 440);
    auto *layout = new QVBoxLayout(this);
    m_title = new QLabel(this);
    layout->addWidget(m_title);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setIndentation(16);
    layout->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemActivated, this, &ReferencesDialog::activate);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &ReferencesDialog::activate);
}

void ReferencesDialog::setReferences(const QString &symbol, const QVector<Entry> &entries, const QString &root)
{
    m_tree->clear();
    QStringList order;
    QHash<QString, QTreeWidgetItem *> files;
    for (const Entry &e : entries) {
        QTreeWidgetItem *&parent = files[e.path];
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree);
            parent->setText(0, root.isEmpty() ? e.path : QDir(root).relativeFilePath(e.path));
            parent->setIcon(0, FileIcons::forFile(QFileInfo(e.path).fileName()));
            QFont f = parent->font(0);
            f.setBold(true);
            parent->setFont(0, f);
            parent->setToolTip(0, e.path);
            order << e.path;
        }
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, QStringLiteral("%1:  %2").arg(e.line + 1).arg(e.text.trimmed()));
        item->setData(0, RolePath, e.path);
        item->setData(0, RoleLine, e.line);
        item->setData(0, RoleColumn, e.column);
    }
    for (const QString &p : std::as_const(order)) {
        QTreeWidgetItem *parent = files.value(p);
        parent->setText(0, QStringLiteral("%1  (%2)").arg(parent->text(0)).arg(parent->childCount()));
    }
    m_tree->expandAll();
    m_title->setText(tr("%n reference(s) to “%1” in %2 file(s)", nullptr, int(entries.size())).arg(symbol).arg(files.size()));
    setWindowTitle(tr("References to %1").arg(symbol));
    if (m_tree->topLevelItemCount() > 0 && m_tree->topLevelItem(0)->childCount() > 0)
        m_tree->setCurrentItem(m_tree->topLevelItem(0)->child(0));
}

void ReferencesDialog::activate(QTreeWidgetItem *item)
{
    if (!item || !item->parent())
        return;
    emit openRequested(item->data(0, RolePath).toString(), item->data(0, RoleLine).toInt(), item->data(0, RoleColumn).toInt());
}
