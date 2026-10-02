#include "UnusedImportsDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

UnusedImportsDialog::UnusedImportsDialog(const QString &fileName, const QVector<int> &lines, const QStringList &texts, QWidget *parent)
    : QDialog(parent), m_list(new QListWidget(this)), m_lines(lines)
{
    setWindowTitle(tr("Remove Unused Imports"));
    resize(520, 360);
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(tr("These imports in <b>%1</b> are not used. Untick any you want to keep.").arg(fileName.toHtmlEscaped()), this);
    info->setWordWrap(true);
    layout->addWidget(info);

    for (const QString &t : texts) {
        auto *item = new QListWidgetItem(t.trimmed(), m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(m_list, 1);

    auto *row = new QHBoxLayout;
    auto *all = new QPushButton(tr("Select All"), this);
    auto *none = new QPushButton(tr("Select None"), this);
    row->addWidget(all);
    row->addWidget(none);
    row->addStretch();
    layout->addLayout(row);
    auto setAll = [this](Qt::CheckState s) {
        for (int i = 0; i < m_list->count(); ++i)
            m_list->item(i)->setCheckState(s);
    };
    connect(all, &QPushButton::clicked, this, [setAll] { setAll(Qt::Checked); });
    connect(none, &QPushButton::clicked, this, [setAll] { setAll(Qt::Unchecked); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *remove = buttons->addButton(tr("Remove Selected"), QDialogButtonBox::AcceptRole);
    remove->setObjectName(QStringLiteral("primaryBtn"));
    remove->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QVector<int> UnusedImportsDialog::selectedLines() const
{
    QVector<int> out;
    for (int i = 0; i < m_list->count() && i < m_lines.size(); ++i)
        if (m_list->item(i)->checkState() == Qt::Checked)
            out.append(m_lines.at(i));
    return out;
}
