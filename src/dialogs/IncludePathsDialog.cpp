#include "IncludePathsDialog.h"

#include "lsp/LspManager.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

IncludePathsDialog::IncludePathsDialog(const QString &projectRoot, const QStringList &paths, const QString &note, QWidget *parent)
    : QDialog(parent), m_root(projectRoot)
{
    setWindowTitle(tr("Include Paths"));
    resize(560, 380);
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(tr("Extra folders searched for <b>#include</b> in this project's C/C++ files. Relative paths are "
                               "relative to the project folder; <b>{project}</b> stands for it. Double-click an entry to edit it."),
                            this);
    info->setWordWrap(true);
    layout->addWidget(info);
    if (!note.isEmpty()) {
        auto *n = new QLabel(note, this);
        n->setWordWrap(true);
        layout->addWidget(n);
    }

    auto *row = new QHBoxLayout;
    m_list = new QListWidget(this);
    m_list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const QString &p : paths)
        addItem(p);
    row->addWidget(m_list, 1);

    auto *side = new QVBoxLayout;
    auto *add = new QPushButton(tr("Add Folder…"), this);
    auto *typed = new QPushButton(tr("Add Path"), this);
    typed->setToolTip(tr("Adds an empty entry to type a path into, e.g. /usr/include/SDL2 or third_party/include"));
    auto *remove = new QPushButton(tr("Remove"), this);
    side->addWidget(add);
    side->addWidget(typed);
    side->addWidget(remove);
    side->addStretch(1);
    row->addLayout(side);
    layout->addLayout(row, 1);

    connect(add, &QPushButton::clicked, this, &IncludePathsDialog::addFolder);
    connect(typed, &QPushButton::clicked, this, &IncludePathsDialog::editSelected);
    connect(remove, &QPushButton::clicked, this, &IncludePathsDialog::removeSelected);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void IncludePathsDialog::addItem(const QString &path)
{
    auto *item = new QListWidgetItem(path, m_list);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
}

void IncludePathsDialog::addFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Add Include Folder"), m_root);
    if (dir.isEmpty())
        return;
    // Folders inside the project are kept relative so the setting survives moving the project.
    QString entry = QDir::cleanPath(dir);
    if (!m_root.isEmpty()) {
        const QString rel = QDir(m_root).relativeFilePath(entry);
        if (!rel.startsWith(QStringLiteral("..")))
            entry = rel;
    }
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->text() == entry)
            return;
    addItem(entry);
    m_list->setCurrentRow(m_list->count() - 1);
}

void IncludePathsDialog::editSelected()
{
    addItem(QString());
    m_list->setCurrentRow(m_list->count() - 1);
    m_list->editItem(m_list->currentItem());
}

void IncludePathsDialog::removeSelected()
{
    qDeleteAll(m_list->selectedItems());
}

QStringList IncludePathsDialog::paths() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i) {
        const QString p = m_list->item(i)->text().trimmed();
        if (!p.isEmpty() && !out.contains(p))
            out << p;
    }
    return out;
}
