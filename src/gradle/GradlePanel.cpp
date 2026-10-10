#include "GradlePanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

namespace {
constexpr int kTaskRole = Qt::UserRole + 1; // the task name on task items; groups have none

QString groupIcon(const QString &title)
{
    const QString t = title.toLower();
    if (t.startsWith(QLatin1String("build")) || t.contains(QLatin1String("release")) || t.contains(QLatin1String("distribution")))
        return QStringLiteral("package");
    if (t.contains(QLatin1String("sign")))
        return QStringLiteral("key");
    if (t.contains(QLatin1String("verif")) || t.contains(QLatin1String("valid")))
        return QStringLiteral("check");
    if (t.contains(QLatin1String("run")) || t.contains(QLatin1String("application")) || t.contains(QLatin1String("compose")))
        return QStringLiteral("play");
    if (t.contains(QLatin1String("help")) || t.contains(QLatin1String("doc")))
        return QStringLiteral("scroll-text");
    if (t.contains(QLatin1String("setup")) || t.contains(QLatin1String("other")))
        return QStringLiteral("cog");
    return QStringLiteral("folder");
}
} // namespace

GradlePanel::GradlePanel(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(tr("GRADLE"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);

    auto button = [this](const QString &icon, const QString &tip) {
        auto *b = new QToolButton(this);
        Icons::bind(b, QStringLiteral(":/new-icons/%1.svg").arg(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    m_refresh = button(QStringLiteral("refresh-cw"), tr("Reload tasks"));
    m_allBtn = button(QStringLiteral("scroll-text"), tr("Also list tasks without a group and those of sub-projects (gradle tasks --all)"));
    m_allBtn->setCheckable(true);
    auto *closeBtn = button(QStringLiteral("x"), tr("Close Gradle panel"));
    connect(m_refresh, &QToolButton::clicked, this, &GradlePanel::reload);
    connect(m_allBtn, &QToolButton::toggled, this, [this](bool on) {
        m_all = on;
        reload();
    });
    connect(closeBtn, &QToolButton::clicked, this, &GradlePanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(m_title, 1);
    hl->addWidget(m_allBtn);
    hl->addWidget(m_refresh);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_folder = new QLabel(this);
    m_folder->setObjectName(QStringLiteral("emptyText"));
    m_folder->setContentsMargins(10, 6, 10, 0);
    m_folder->setTextInteractionFlags(Qt::NoTextInteraction);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter tasks"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &GradlePanel::applyFilter);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(14);
    m_tree->setRootIsDecorated(true);
    m_tree->setIconSize(QSize(14, 14));
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setUniformRowHeights(true);
    m_tree->setMouseTracking(true);
    m_tree->installEventFilter(this);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &GradlePanel::showContextMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) { runTask(it); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &GradlePanel::updateDetail);

    m_stateTitle = new QLabel(this);
    m_stateTitle->setAlignment(Qt::AlignCenter);
    QFont f = m_stateTitle->font();
    f.setBold(true);
    m_stateTitle->setFont(f);
    m_stateText = new QLabel(this);
    m_stateText->setObjectName(QStringLiteral("emptyText"));
    m_stateText->setAlignment(Qt::AlignCenter);
    m_stateText->setWordWrap(true);
    m_stateText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_retry = new QToolButton(this);
    m_retry->setText(tr("Try again"));
    m_retry->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_retry, &QToolButton::clicked, this, &GradlePanel::reload);
    auto *state = new QWidget(this);
    auto *sl = new QVBoxLayout(state);
    sl->setContentsMargins(18, 18, 18, 18);
    sl->addStretch(1);
    sl->addWidget(m_stateTitle);
    sl->addWidget(m_stateText);
    sl->addWidget(m_retry, 0, Qt::AlignHCenter);
    sl->addStretch(2);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_tree); // 0
    m_stack->addWidget(state);  // 1

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("emptyText"));
    m_detail->setWordWrap(true);
    m_detail->setContentsMargins(10, 6, 10, 8);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setStyleSheet(QStringLiteral("QLabel { border-top: 1px solid palette(shadow); }"));

    auto *filterRow = new QWidget(this);
    auto *fl = new QHBoxLayout(filterRow);
    fl->setContentsMargins(8, 6, 8, 6);
    fl->addWidget(m_filter);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_folder);
    layout->addWidget(filterRow);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_detail);

    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { populate(); });
    updateDetail();
}

void GradlePanel::setBuildDir(const QString &dir)
{
    if (dir == m_dir)
        return;
    m_dir = dir;
    m_groups.clear();
    m_loadedStamp = -1;
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_tree->clear();
    m_folder->setText(dir.isEmpty() ? QString() : QDir::toNativeSeparators(QDir::home().relativeFilePath(dir)).prepend(QStringLiteral("~/")));
    updateDetail();
}

qint64 GradlePanel::buildStamp() const
{
    qint64 stamp = 0;
    for (const QString &f : {QStringLiteral("settings.gradle"), QStringLiteral("settings.gradle.kts"), QStringLiteral("build.gradle"),
                             QStringLiteral("build.gradle.kts"), QStringLiteral("gradle/libs.versions.toml")})
        stamp = qMax(stamp, QFileInfo(m_dir + QLatin1Char('/') + f).lastModified().toMSecsSinceEpoch());
    return stamp;
}

void GradlePanel::ensureLoaded()
{
    if (m_dir.isEmpty() || m_proc)
        return;
    if (m_groups.isEmpty() || m_loadedStamp != buildStamp())
        reload();
}

void GradlePanel::reload()
{
    if (m_dir.isEmpty())
        return;
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
    }
    showState(tr("Loading tasks…"), tr("Gradle is reading the build. The first run can take a while."), false);
    m_refresh->setEnabled(false);
    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_dir);
    QProcess *proc = m_proc;
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(proc->readAllStandardOutput());
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        proc->deleteLater();
        m_proc = nullptr;
        m_refresh->setEnabled(true);
        if (code != 0) {
            const QStringList lines = (err.isEmpty() ? out.trimmed() : err).split(QLatin1Char('\n'));
            showState(tr("Gradle could not list the tasks"), lines.mid(0, 8).join(QLatin1Char('\n')), true);
            return;
        }
        m_groups = GradleTasks::parse(out);
        m_loadedStamp = buildStamp();
        if (m_groups.isEmpty()) {
            showState(tr("No tasks found"), tr("Gradle printed no task list for this project."), true);
            return;
        }
        populate();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        proc->deleteLater();
        m_proc = nullptr;
        m_refresh->setEnabled(true);
        showState(tr("Could not start Gradle"), tr("The project has no executable gradlew and gradle is not on PATH."), true);
    });
    QStringList args = {QStringLiteral("-q"), QStringLiteral("--console=plain"), QStringLiteral("tasks")};
    if (m_all)
        args << QStringLiteral("--all");
    const QString runner = GradleTasks::runner(m_dir);
    proc->start(runner == QLatin1String("./gradlew") ? m_dir + QStringLiteral("/gradlew") : runner, args);
}

void GradlePanel::showState(const QString &title, const QString &text, bool retry)
{
    m_stateTitle->setText(title);
    m_stateText->setText(text);
    m_retry->setVisible(retry);
    m_stack->setCurrentIndex(1);
    m_detail->hide();
}

void GradlePanel::populate()
{
    if (m_groups.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QString filter = m_filter->text();
    m_tree->clear();
    QFont bold = m_tree->font();
    bold.setBold(true);
    for (const GradleTasks::Group &g : m_groups) {
        auto *top = new QTreeWidgetItem(m_tree);
        top->setText(0, g.title);
        top->setFont(0, bold);
        top->setIcon(0, Icons::tinted(QStringLiteral(":/new-icons/%1.svg").arg(groupIcon(g.title)), t.accent, t.textMuted));
        top->setToolTip(0, tr("%n task(s)", nullptr, int(g.tasks.size())));
        top->setFlags(Qt::ItemIsEnabled);
        for (const GradleTasks::Task &task : g.tasks) {
            auto *it = new QTreeWidgetItem(top);
            it->setText(0, task.name);
            it->setData(0, kTaskRole, task.name);
            it->setToolTip(0, task.description.isEmpty() ? task.name : task.name + QStringLiteral(" — ") + task.description);
            it->setIcon(0, Icons::tinted(QStringLiteral(":/new-icons/play.svg"), t.textMuted, t.textMuted));
        }
        top->setExpanded(true);
    }
    m_stack->setCurrentIndex(0);
    m_detail->show();
    if (!filter.isEmpty())
        applyFilter(filter);
    updateDetail();
}

void GradlePanel::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *g = m_tree->topLevelItem(i);
        int shown = 0;
        for (int j = 0; j < g->childCount(); ++j) {
            QTreeWidgetItem *c = g->child(j);
            const bool match = needle.isEmpty() || c->text(0).contains(needle, Qt::CaseInsensitive) ||
                               c->toolTip(0).contains(needle, Qt::CaseInsensitive);
            c->setHidden(!match);
            shown += match;
        }
        g->setHidden(shown == 0);
        if (!needle.isEmpty())
            g->setExpanded(true);
    }
}

QString GradlePanel::commandFor(QTreeWidgetItem *item, const QString &extraArgs) const
{
    const QString task = item ? item->data(0, kTaskRole).toString() : QString();
    if (task.isEmpty())
        return {};
    return GradleTasks::runner(m_dir) + QLatin1Char(' ') + task + (extraArgs.trimmed().isEmpty() ? QString() : QLatin1Char(' ') + extraArgs.trimmed());
}

void GradlePanel::runTask(QTreeWidgetItem *item, const QString &extraArgs)
{
    const QString cmd = commandFor(item, extraArgs);
    if (!cmd.isEmpty())
        emit runRequested(cmd, m_dir);
}

void GradlePanel::updateDetail()
{
    QTreeWidgetItem *it = m_tree->currentItem();
    const QString task = it ? it->data(0, kTaskRole).toString() : QString();
    if (task.isEmpty()) {
        m_detail->setText(tr("Double-click a task to run it in the terminal."));
        return;
    }
    QString description = it->toolTip(0);
    const int dash = description.indexOf(QStringLiteral(" — "));
    description = dash >= 0 ? description.mid(dash + 3) : QString();
    m_detail->setText(commandFor(it) + (description.isEmpty() ? QString() : QLatin1Char('\n') + description));
}

void GradlePanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_tree->itemAt(pos);
    if (!it || it->data(0, kTaskRole).toString().isEmpty())
        return;
    m_tree->setCurrentItem(it);
    QMenu menu(this);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    auto icon = [&](const char *n) { return Icons::tinted(QStringLiteral(":/new-icons/%1.svg").arg(QLatin1String(n)), t.editorFg, t.textMuted); };
    menu.addAction(icon("play"), tr("Run"), this, [this, it] { runTask(it); });
    menu.addAction(icon("pencil"), tr("Run with Arguments…"), this, [this, it] {
        bool ok = false;
        const QString args = QInputDialog::getText(this, tr("Run with Arguments"), tr("Arguments for %1 (for example --info or --stacktrace):").arg(it->text(0)),
                                                   QLineEdit::Normal, QString(), &ok);
        if (ok)
            runTask(it, args);
    });
    menu.addSeparator();
    menu.addAction(icon("copy"), tr("Copy Command"), this, [this, it] { QApplication::clipboard()->setText(commandFor(it)); });
    menu.addAction(icon("copy"), tr("Copy Task Name"), this, [it] { QApplication::clipboard()->setText(it->text(0)); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

bool GradlePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && m_tree->currentItem()) {
            runTask(m_tree->currentItem());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
