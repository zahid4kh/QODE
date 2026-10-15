#include "spring/AddStarterDialog.h"

#include "spring/SpringStarters.h"

#include <QCursor>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole;
}

AddStarterDialog::AddStarterDialog(const SpringProject &project, QWidget *parent) : QDialog(parent), m_project(project)
{
    setWindowTitle(tr("Add Spring Starters"));
    resize(560, 560);
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(tr("Adds the dependencies to <b>%1</b>. Versions are managed by Spring Boot.").arg(QFileInfo(project.buildFile()).fileName().toHtmlEscaped()), this);
    info->setWordWrap(true);
    layout->addWidget(info);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search starters…"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(14);
    m_tree->setUniformRowHeights(false);
    m_tree->setWordWrap(true);
    m_tree->setAlternatingRowColors(false);
    layout->addWidget(m_tree, 1);

    const QStringList have = SpringStarters::installed(project);
    QMap<QString, QTreeWidgetItem *> groups;
    for (const SpringStarters::Starter &s : SpringStarters::catalog()) {
        QTreeWidgetItem *&group = groups[s.category];
        if (!group) {
            group = new QTreeWidgetItem(m_tree, QStringList{s.category});
            QFont f = group->font(0);
            f.setBold(true);
            group->setFont(0, f);
            group->setFlags(Qt::ItemIsEnabled);
            group->setExpanded(true);
        }
        const bool present = have.contains(s.id);
        auto *item = new QTreeWidgetItem(group);
        item->setText(0, present ? tr("%1 — already added").arg(s.title) : s.title);
        item->setToolTip(0, s.description);
        item->setData(0, kIdRole, s.id);
        item->setData(0, Qt::UserRole + 1, s.title + QLatin1Char(' ') + s.description);
        item->setFlags(present ? Qt::ItemIsUserCheckable : (Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable));
        item->setCheckState(0, present ? Qt::Checked : Qt::Unchecked);
        // Description under the title.
        item->setText(0, item->text(0) + QStringLiteral("\n") + s.description);
    }

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(tr("Add to Build File"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &AddStarterDialog::filter);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { updateSummary(); });
    // Clicking the text toggles too; the check box itself is toggled by the view.
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        if (!it->parent() || !(it->flags() & Qt::ItemIsEnabled))
            return;
        const QPoint pos = m_tree->viewport()->mapFromGlobal(QCursor::pos());
        if (pos.x() < m_tree->visualItemRect(it).left() + 24)
            return;
        it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    updateSummary();
}

QStringList AddStarterDialog::selectedIds() const
{
    QStringList ids;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem *it = group->child(i);
            if ((it->flags() & Qt::ItemIsEnabled) && it->checkState(0) == Qt::Checked)
                ids << it->data(0, kIdRole).toString();
        }
    }
    return ids;
}

void AddStarterDialog::filter(const QString &text)
{
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_tree->topLevelItem(g);
        int visible = 0;
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem *it = group->child(i);
            const bool show = text.isEmpty() || it->data(0, Qt::UserRole + 1).toString().contains(text, Qt::CaseInsensitive);
            it->setHidden(!show);
            visible += show ? 1 : 0;
        }
        group->setHidden(visible == 0);
    }
}

void AddStarterDialog::updateSummary()
{
    const int n = selectedIds().size();
    m_summary->setText(n == 0 ? tr("Tick the starters to add.") : tr("%n starter(s) will be added.", nullptr, n));
    m_ok->setEnabled(n > 0);
}
