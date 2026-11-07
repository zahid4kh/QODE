#include "dialogs/QodeFilesDialog.h"

#include "filesystem/FileManager.h"
#include "platform/Platform.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum Role { PathRole = Qt::UserRole, OpenableRole };
enum Column { LocationCol, SizeCol, WhatCol };

QString sizeText(qint64 bytes)
{
    return bytes < 0 ? QString() : QLocale().formattedDataSize(bytes);
}

QodeFilesDialog::Scan scan()
{
    QodeFilesDialog::Scan s;
    s.items = QodeFiles::created(true);
    for (const QodeFiles::Item &item : std::as_const(s.items)) {
        if (!item.folder)
            continue;
        QList<QodeFilesDialog::Entry> entries;
        for (const QFileInfo &e : QDir(item.path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            const bool link = e.isSymLink() || e.isJunction();
            entries.append({e.fileName(), link ? 0 : QodeFiles::sizeOf(e.absoluteFilePath()), e.isDir() && !link});
        }
        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.size > b.size; });
        s.children.insert(item.path, entries);
    }
    return s;
}

QTreeWidgetItem *section(QTreeWidget *tree, const QString &title)
{
    auto *item = new QTreeWidgetItem(tree, {title});
    QFont bold = item->font(LocationCol);
    bold.setBold(true);
    item->setFont(LocationCol, bold);
    item->setFont(SizeCol, bold);
    item->setExpanded(true);
    return item;
}

} // namespace

QodeFilesDialog::QodeFilesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("QODE's Files"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    const bool windows = Platform::osTag() == QLatin1String("win");
    auto *intro = new QLabel(windows ? tr("Everything QODE has put on this computer. QODE writes nowhere else: no other folders, "
                                          "and no registry entries besides the installer's, listed here.")
                                     : tr("Everything QODE has put on this computer. QODE writes nowhere else."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Location"), tr("Size"), tr("What it is")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(LocationCol, QHeaderView::Interactive);
    m_tree->setColumnWidth(LocationCol, 420);
    m_tree->setColumnWidth(SizeCol, 90);
    layout->addWidget(m_tree, 1);

    m_created = section(m_tree, tr("Created while you use QODE"));
    m_installed = section(m_tree, windows ? tr("Installed with QODE") : tr("Installed with QODE (package)"));
    for (const Platform::InstalledItem &i : Platform::installedItems()) {
        auto *row = new QTreeWidgetItem(m_installed, {i.registry ? i.path : QDir::toNativeSeparators(i.path), QString(), i.what});
        row->setData(LocationCol, PathRole, i.path);
        row->setData(LocationCol, OpenableRole, !i.registry);
        row->setToolTip(WhatCol, i.what);
        if (i.registry)
            row->setToolTip(LocationCol, tr("A registry key (regedit shows it)"));
    }

    auto *hint = new QLabel(Platform::removalHint(), this);
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    layout->addWidget(hint);
    QString toolsText = tr("Tools QODE starts for your projects (Gradle, Maven, npm, pip, git, the Claude Code CLI) keep "
                           "their own downloads and caches where they always do (~/.gradle, ~/.m2, npm's cache …); those "
                           "belong to the tools, not to QODE.");
    if (windows)
        toolsText += QLatin1Char(' ') + tr("Windows itself also keeps a few records about every program you run (taskbar, "
                                           "notification settings, recent folders in file dialogs); QODE does not write those.");
    auto *tools = new QLabel(toolsText, this);
    tools->setWordWrap(true);
    layout->addWidget(tools);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_open = buttons->addButton(tr("Open"), QDialogButtonBox::ActionRole);
    m_copy = buttons->addButton(tr("Copy Path"), QDialogButtonBox::ActionRole);
    QPushButton *copyAll = buttons->addButton(tr("Copy List"), QDialogButtonBox::ActionRole);
    m_open->setEnabled(false);
    m_copy->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_open, &QPushButton::clicked, this, &QodeFilesDialog::openSelected);
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(QDir::toNativeSeparators(selectedPath())); });
    connect(copyAll, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(listText()); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QTreeWidgetItem *item = m_tree->currentItem();
        m_open->setEnabled(item && item->data(LocationCol, OpenableRole).toBool());
        m_copy->setEnabled(!selectedPath().isEmpty());
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { openSelected(); });

    // The list at once, the sizes when counted (gigabytes of language servers take a moment).
    Scan quick;
    quick.items = QodeFiles::created(false);
    showCreated(quick, false);
    QPointer<QodeFilesDialog> self(this);
    QThread *worker = QThread::create([self] {
        const Scan s = scan();
        QMetaObject::invokeMethod(qApp, [self, s] {
            if (self)
                self->showCreated(s, true);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);

    resize(980, 600);
}

void QodeFilesDialog::showCreated(const Scan &scan, bool counted)
{
    const QString selected = selectedPath();
    qDeleteAll(m_created->takeChildren());
    qint64 total = 0;
    for (const QodeFiles::Item &item : scan.items) {
        auto *row = new QTreeWidgetItem(m_created, {QDir::toNativeSeparators(item.path), counted ? sizeText(item.size) : tr("counting…"), item.what});
        row->setData(LocationCol, PathRole, item.path);
        row->setData(LocationCol, OpenableRole, true);
        row->setTextAlignment(SizeCol, Qt::AlignRight | Qt::AlignVCenter);
        row->setToolTip(WhatCol, item.what); // the column is often narrower than the text
        total += qMax<qint64>(item.size, 0);
        for (const Entry &e : scan.children.value(item.path)) {
            const QString path = item.path + QLatin1Char('/') + e.name;
            auto *child = new QTreeWidgetItem(row, {e.folder ? e.name + QDir::separator() : e.name, sizeText(e.size)});
            child->setData(LocationCol, PathRole, path);
            child->setData(LocationCol, OpenableRole, true);
            child->setTextAlignment(SizeCol, Qt::AlignRight | Qt::AlignVCenter);
        }
        if (!selected.isEmpty() && Platform::samePath(selected, item.path))
            m_tree->setCurrentItem(row);
    }
    if (scan.items.isEmpty())
        new QTreeWidgetItem(m_created, {tr("Nothing yet")});
    m_created->setText(SizeCol, counted ? sizeText(total) : QString());
    m_created->setTextAlignment(SizeCol, Qt::AlignRight | Qt::AlignVCenter);
}

QString QodeFilesDialog::selectedPath() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    return item ? item->data(LocationCol, PathRole).toString() : QString();
}

void QodeFilesDialog::openSelected()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || !item->data(LocationCol, OpenableRole).toBool())
        return;
    const QString path = item->data(LocationCol, PathRole).toString();
    if (QFileInfo(path).isDir())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    else
        FileManager::revealInFileManager(path);
}

QString QodeFilesDialog::listText() const
{
    QStringList lines;
    for (const QTreeWidgetItem *sec : {m_created, m_installed}) {
        lines << sec->text(LocationCol) + (sec->text(SizeCol).isEmpty() ? QString() : QStringLiteral(" (%1)").arg(sec->text(SizeCol)));
        for (int i = 0; i < sec->childCount(); ++i) {
            const QTreeWidgetItem *row = sec->child(i);
            QString line = QStringLiteral("  ") + row->text(LocationCol);
            if (!row->text(SizeCol).isEmpty())
                line += QStringLiteral("  (%1)").arg(row->text(SizeCol));
            if (!row->text(WhatCol).isEmpty())
                line += QStringLiteral("  ") + row->text(WhatCol);
            lines << line;
        }
        lines << QString();
    }
    return lines.join(QLatin1Char('\n')).trimmed() + QLatin1Char('\n');
}
