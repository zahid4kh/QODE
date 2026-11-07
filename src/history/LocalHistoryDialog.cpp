#include "LocalHistoryDialog.h"
#include "editor/TextEncoding.h"

#include "filesystem/FileManager.h"
#include "git/DiffPane.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

constexpr int kPathRole = Qt::UserRole + 1;

QString when(qint64 msecs)
{
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(msecs);
    const QDate today = QDate::currentDate();
    const QString time = QLocale().toString(t.time(), QStringLiteral("HH:mm:ss"));
    if (t.date() == today)
        return LocalHistoryDialog::tr("Today %1").arg(time);
    if (t.date() == today.addDays(-1))
        return LocalHistoryDialog::tr("Yesterday %1").arg(time);
    return QLocale().toString(t.date(), QStringLiteral("d MMM")) + QLatin1Char(' ') + time;
}

} // namespace

LocalHistoryDialog::LocalHistoryDialog(const QString &path, const Hooks &hooks, QWidget *parent)
    : QDialog(parent, Qt::Window)
    , m_root(QDir::cleanPath(path))
    , m_folder(QFileInfo(path).isDir())
    , m_hooks(hooks)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1280, 760);
    setWindowTitle(tr("Local History — %1").arg(QFileInfo(m_root).fileName()));

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_stats = new QLabel(this);
    m_stats->setObjectName(QStringLiteral("emptyText"));
    m_compare = new QComboBox(this);
    m_compare->addItem(tr("Compare with the current text"));
    m_compare->addItem(tr("Compare with the version before"));
    m_prev = new QPushButton(tr("▲ Previous"), this);
    m_next = new QPushButton(tr("▼ Next"), this);
    m_restore = new QPushButton(tr("Restore This Version"), this);
    m_restore->setToolTip(tr("Puts this version back. The text it replaces is kept in the history too."));

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 6, 8, 6);
    bar->addWidget(m_stats, 1);
    bar->addWidget(m_compare);
    bar->addWidget(m_prev);
    bar->addWidget(m_next);
    bar->addWidget(m_restore);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("historyVersions"));
    m_pane = new DiffPane(this);

    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addLayout(bar);
    rightLayout->addWidget(m_pane, 1);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    if (m_folder) {
        auto *left = new QWidget(this);
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(6, 6, 0, 6);
        m_filter = new QLineEdit(left);
        m_filter->setPlaceholderText(tr("Filter files"));
        m_filter->setClearButtonEnabled(true);
        m_files = new QListWidget(left);
        leftLayout->addWidget(m_filter);
        leftLayout->addWidget(m_files, 1);
        split->addWidget(left);
        connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
            for (int i = 0; i < m_files->count(); ++i)
                m_files->item(i)->setHidden(!m_files->item(i)->text().contains(text, Qt::CaseInsensitive));
        });
        connect(m_files, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
            if (item)
                showFile(item->data(kPathRole).toString());
        });
    }
    split->addWidget(m_list);
    split->addWidget(right);
    split->setStretchFactor(split->count() - 1, 1);
    split->setSizes(m_folder ? QList<int>{300, 230, 750} : QList<int>{240, 1040});

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_title);
    root->addWidget(split, 1);

    connect(m_list, &QListWidget::currentRowChanged, this, &LocalHistoryDialog::showVersion);
    connect(m_compare, &QComboBox::currentIndexChanged, this, &LocalHistoryDialog::showVersion);
    connect(m_prev, &QPushButton::clicked, this, [this] { m_pane->gotoChange(false); });
    connect(m_next, &QPushButton::clicked, this, [this] { m_pane->gotoChange(true); });
    connect(m_restore, &QPushButton::clicked, this, &LocalHistoryDialog::restoreSelected);

    if (m_folder)
        loadFiles();
    else
        showFile(m_root);
}

void LocalHistoryDialog::loadFiles()
{
    const QVector<LocalHistory::TrackedFile> files = LocalHistory::filesUnder(m_root);
    const QColor muted = Theme::byName(SettingsManager::instance().theme()).textMuted;
    m_files->clear();
    for (const LocalHistory::TrackedFile &f : files) {
        QString text = QDir(m_root).relativeFilePath(f.path);
        if (!f.exists)
            text += tr("  (deleted)");
        auto *item = new QListWidgetItem(text, m_files);
        item->setData(kPathRole, f.path);
        item->setToolTip(tr("%1\n%n version(s), the newest %2", nullptr, f.versions).arg(FileManager::displayPath(f.path), when(f.newest)));
        if (!f.exists)
            item->setForeground(muted);
    }
    if (m_files->count() > 0) {
        m_files->setCurrentRow(0);
    } else {
        m_title->setText(tr("No local history under %1 yet: versions are kept from the first save.").arg(FileManager::displayPath(m_root)));
        m_list->clear();
        m_pane->setMessage(QString());
        m_restore->setEnabled(false);
        m_compare->setEnabled(false);
        m_prev->setEnabled(false);
        m_next->setEnabled(false);
    }
}

void LocalHistoryDialog::showFile(const QString &path)
{
    m_file = path;
    m_versions = LocalHistory::versions(path);
    m_title->setText(FileManager::displayPath(path));
    m_list->clear();
    for (const LocalHistory::Version &v : std::as_const(m_versions)) {
        auto *item = new QListWidgetItem(when(v.time) + QStringLiteral("   ") + LocalHistory::label(v.kind), m_list);
        item->setToolTip(QLocale().toString(QDateTime::fromMSecsSinceEpoch(v.time), QLocale::LongFormat));
    }
    if (m_versions.isEmpty()) {
        m_pane->setMessage(tr("No versions of this file yet: they are kept from the first save."));
        m_stats->clear();
        m_restore->setEnabled(false);
        m_prev->setEnabled(false);
        m_next->setEnabled(false);
        return;
    }
    m_list->setCurrentRow(0);
}

void LocalHistoryDialog::showVersion()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_versions.size())
        return;
    const LocalHistory::Version &v = m_versions.at(row);
    const QString text = TextEncoding::decode(LocalHistory::content(v)).text;
    const QString name = when(v.time) + QStringLiteral(" — ") + LocalHistory::label(v.kind);
    DiffPane::Stats st;
    if (m_compare->currentIndex() == 1) {
        const bool first = row + 1 >= m_versions.size();
        const QString before = first ? QString() : TextEncoding::decode(LocalHistory::content(m_versions.at(row + 1))).text;
        m_pane->setTitles(first ? tr("(no older version)") : when(m_versions.at(row + 1).time) + QStringLiteral(" — ") + LocalHistory::label(m_versions.at(row + 1).kind), name);
        st = m_pane->setTexts(before, text);
    } else {
        bool exists = true;
        const QString now = m_hooks.currentText ? m_hooks.currentText(m_file, &exists) : QString();
        m_pane->setTitles(name, exists ? tr("Current") : tr("Current (the file is deleted)"));
        st = m_pane->setTexts(text, now);
    }
    m_pane->firstChange();
    m_stats->setText(st.hunks == 0 ? tr("No differences.") : tr("%n change(s):  +%1  −%2", nullptr, st.hunks).arg(st.added).arg(st.removed));
    m_prev->setEnabled(st.hunks > 0);
    m_next->setEnabled(st.hunks > 0);
    m_restore->setEnabled(true);
}

void LocalHistoryDialog::restoreSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_versions.size() || !m_hooks.restore)
        return;
    const QByteArray data = LocalHistory::content(m_versions.at(row));
    if (!m_hooks.restore(m_file, TextEncoding::decode(data).text, data)) {
        QMessageBox::warning(this, tr("Local History"), tr("Could not restore %1.").arg(FileManager::displayPath(m_file)));
        return;
    }
    const QString file = m_file;
    if (m_folder) {
        loadFiles();
        for (int i = 0; i < m_files->count(); ++i)
            if (m_files->item(i)->data(kPathRole).toString() == file)
                m_files->setCurrentRow(i);
    } else {
        showFile(file);
    }
}
