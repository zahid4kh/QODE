#include "gradle/GradleBuildView.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kMsRole = Qt::UserRole + 1;
constexpr int kMaxRole = Qt::UserRole + 2;
constexpr int kPathRole = Qt::UserRole + 3;
constexpr int kLineRole = Qt::UserRole + 4;
constexpr int kColumnRole = Qt::UserRole + 5;

QString seconds(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    if (ms < 60000)
        return QStringLiteral("%1 s").arg(QString::number(ms / 1000.0, 'f', ms < 10000 ? 2 : 1));
    return QStringLiteral("%1 min %2 s").arg(ms / 60000).arg((ms % 60000) / 1000);
}

// Time column: the duration as text over a bar scaled to the slowest task.
class BarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const qint64 ms = index.data(kMsRole).toLongLong(), max = index.data(kMaxRole).toLongLong();
        if (max > 0 && ms > 0) {
            const Theme t = Theme::byName(SettingsManager::instance().theme());
            QColor c = t.accent;
            c.setAlpha(70);
            QRect r = option.rect.adjusted(2, 3, -2, -3);
            r.setWidth(qMax(2, int(r.width() * double(ms) / double(max))));
            p->fillRect(r, c);
        }
        QStyledItemDelegate::paint(p, option, index);
    }
};
} // namespace

GradleBuildView::GradleBuildView(QWidget *parent)
    : QWidget(parent)
    , m_poll(new QTimer(this))
{
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setContentsMargins(10, 8, 10, 6);
    m_summary->setTextFormat(Qt::RichText);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("Task"), tr("Result"), tr("Time")});
    m_tree->setIndentation(14);
    m_tree->setUniformRowHeights(true);
    m_tree->setIconSize(QSize(12, 12));
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->resizeSection(2, 90);
    m_tree->setItemDelegateForColumn(2, new BarDelegate(m_tree));
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        const QString path = it->data(0, kPathRole).toString();
        if (!path.isEmpty())
            emit locationRequested(path, it->data(0, kLineRole).toInt(), it->data(0, kColumnRole).toInt());
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_summary);
    layout->addWidget(m_tree, 1);

    m_poll->setInterval(400);
    connect(m_poll, &QTimer::timeout, this, &GradleBuildView::refresh);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &GradleBuildView::populate);
    populate();
}

void GradleBuildView::setRoot(const QString &root)
{
    if (root == m_root)
        return;
    m_root = root;
    m_build = {};
    m_problems.clear();
    m_poll->stop();
    if (!root.isEmpty())
        refresh();
    else
        populate();
}

void GradleBuildView::follow()
{
    if (m_root.isEmpty())
        return;
    m_followSince = QDateTime::currentMSecsSinceEpoch();
    m_problems.clear();
    m_announced = false;
    m_build = {};
    m_build.start = m_followSince; // shown as "running" until the first event
    populate();
    m_poll->start();
}

void GradleBuildView::refresh()
{
    if (m_root.isEmpty())
        return;
    QFile f(GradleEvents::eventsFile(m_root));
    if (!f.open(QIODevice::ReadOnly)) {
        if (!m_poll->isActive())
            populate();
        return;
    }
    const GradleEvents::Build b = GradleEvents::parseLast(QString::fromUtf8(f.readAll()));
    // Still the previous build in the file: keep waiting (a configuration cache hit writes no start line).
    if (m_poll->isActive() && (b.start == 0 || b.start < m_followSince - 2000)) {
        if (QDateTime::currentMSecsSinceEpoch() - m_followSince > 15 * 60 * 1000)
            m_poll->stop();
        return;
    }
    const bool changed = b.tasks.size() != m_build.tasks.size() || b.end != m_build.end || b.start != m_build.start;
    m_build = b;
    if (b.end > 0) {
        m_poll->stop();
        if (!m_announced) {
            m_announced = true;
            emit buildFinished(b.failed());
        }
    }
    if (changed)
        populate();
}

void GradleBuildView::addProblems(const QList<BuildProblem> &problems)
{
    if (m_followSince == 0)
        return;
    for (const BuildProblem &p : problems)
        if (m_problems.size() < 200)
            m_problems << p;
    populate();
}

void GradleBuildView::populate()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_tree->clear();
    if (m_build.tasks.isEmpty() && m_build.start == 0) {
        m_summary->setText(QStringLiteral("<span style=\"color:%1\">%2</span>")
                               .arg(t.textMuted.name(), tr("Run a task (here, F5, or a ▶ in the editor) to see its tasks, their results and times.")));
        return;
    }
    int executed = 0, upToDate = 0, cached = 0, skipped = 0, failed = 0;
    qint64 max = 0;
    for (const GradleEvents::Task &task : std::as_const(m_build.tasks)) {
        max = qMax(max, task.ms());
        if (task.state == QLatin1String("EXECUTED"))
            ++executed;
        else if (task.state == QLatin1String("UP-TO-DATE"))
            ++upToDate;
        else if (task.state == QLatin1String("FROM-CACHE"))
            ++cached;
        else if (task.state == QLatin1String("SKIPPED"))
            ++skipped;
        else if (task.state == QLatin1String("FAILED"))
            ++failed;
    }
    const bool running = m_build.end == 0;
    const qint64 total = (running ? QDateTime::currentMSecsSinceEpoch() : m_build.end) - m_build.start;
    const QString head = running ? QStringLiteral("<b>%1</b>").arg(tr("Building…"))
                         : failed ? QStringLiteral("<b style=\"color:%1\">%2</b>").arg(t.danger.name(), tr("BUILD FAILED"))
                                  : QStringLiteral("<b style=\"color:%1\">%2</b>").arg(t.success.name(), tr("BUILD SUCCESSFUL"));
    QStringList counts;
    if (executed)
        counts << tr("%1 executed").arg(executed);
    if (upToDate)
        counts << tr("%1 up-to-date").arg(upToDate);
    if (cached)
        counts << tr("%1 from cache").arg(cached);
    if (skipped)
        counts << tr("%1 skipped").arg(skipped);
    if (failed)
        counts << tr("%1 failed").arg(failed);
    m_summary->setText(QStringLiteral("%1 · %2<br><span style=\"color:%3\">%4</span>")
                           .arg(head, seconds(qMax<qint64>(0, total)), t.textMuted.name(),
                                counts.isEmpty() ? tr("waiting for the first task…") : counts.join(QStringLiteral(" · "))));

    auto colorOf = [&t](const QString &state) {
        if (state == QLatin1String("EXECUTED"))
            return t.success;
        if (state == QLatin1String("FROM-CACHE"))
            return t.accent;
        if (state == QLatin1String("FAILED"))
            return t.danger;
        return t.textMuted;
    };
    QTreeWidgetItem *lastFailed = nullptr;
    for (const GradleEvents::Task &task : std::as_const(m_build.tasks)) {
        auto *it = new QTreeWidgetItem(m_tree, {task.path, task.state == QLatin1String("SKIPPED") && !task.detail.isEmpty() ? task.detail : task.state,
                                                task.ms() > 0 ? seconds(task.ms()) : QString()});
        it->setForeground(1, colorOf(task.state));
        if (task.state == QLatin1String("UP-TO-DATE") || task.state == QLatin1String("SKIPPED"))
            it->setForeground(0, t.textMuted);
        it->setData(2, kMsRole, task.ms());
        it->setData(2, kMaxRole, max);
        it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        if (task.state == QLatin1String("FAILED")) {
            it->setIcon(0, Icons::tinted(QStringLiteral(":/new-icons/circle-x.svg"), t.danger));
            if (!task.detail.isEmpty()) {
                auto *d = new QTreeWidgetItem(it, {task.detail});
                d->setToolTip(0, task.detail);
                d->setForeground(0, t.danger);
                d->setFirstColumnSpanned(true);
            }
            it->setExpanded(true);
            lastFailed = it;
        }
    }
    if (!m_problems.isEmpty()) {
        QTreeWidgetItem *parent = lastFailed;
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree, {tr("Compiler messages")});
            parent->setFirstColumnSpanned(true);
        }
        for (const BuildProblem &p : std::as_const(m_problems)) {
            QString where = m_root.isEmpty() ? p.path : QDir(m_root).relativeFilePath(p.path);
            if (p.line > 0)
                where += QLatin1Char(':') + QString::number(p.line);
            auto *c = new QTreeWidgetItem(parent, {where + QStringLiteral("  ") + p.message});
            c->setFirstColumnSpanned(true);
            c->setToolTip(0, p.message);
            c->setIcon(0, Icons::tinted(p.error ? QStringLiteral(":/new-icons/circle-x.svg") : QStringLiteral(":/new-icons/triangle-alert.svg"),
                                        p.error ? t.danger : t.warning));
            c->setData(0, kPathRole, p.path);
            c->setData(0, kLineRole, p.line);
            c->setData(0, kColumnRole, qMax(1, p.column));
        }
        parent->setExpanded(true);
    }
    if (running && m_tree->topLevelItemCount() > 0)
        m_tree->scrollToItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
}
