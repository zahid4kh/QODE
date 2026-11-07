#include "GradlePanel.h"
#include "platform/Platform.h"

#include "gradle/GradleBuildView.h"
#include "jvm/JvmTools.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTabBar>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
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

    // The wrapper: its Gradle version, an upgrade to the current release, and the exec bit a zip download loses.
    m_wrapperRow = new QWidget(this);
    auto *wl = new QHBoxLayout(m_wrapperRow);
    wl->setContentsMargins(10, 4, 6, 0);
    wl->setSpacing(4);
    m_wrapperLabel = new QLabel(m_wrapperRow);
    m_wrapperLabel->setObjectName(QStringLiteral("emptyText"));
    m_upgradeBtn = new QToolButton(m_wrapperRow);
    m_upgradeBtn->setText(tr("Upgrade…"));
    m_upgradeBtn->setToolTip(tr("Upgrade the Gradle wrapper to the current Gradle release"));
    m_upgradeBtn->setAutoRaise(true);
    connect(m_upgradeBtn, &QToolButton::clicked, this, &GradlePanel::upgradeWrapper);
    m_execBtn = new QToolButton(m_wrapperRow);
    m_execBtn->setText(tr("Make gradlew executable"));
    m_execBtn->setToolTip(tr("gradlew has lost its exec bit (a zip download, or a checkout without file modes): chmod +x gradlew"));
    m_execBtn->setAutoRaise(true);
    connect(m_execBtn, &QToolButton::clicked, this, [this] {
        QFile f(m_dir + QStringLiteral("/gradlew"));
        if (!f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther))
            QMessageBox::warning(this, tr("Gradle"), tr("Could not change the mode of gradlew: %1").arg(f.errorString()));
        updateWrapper();
        reload();
    });
    wl->addWidget(m_wrapperLabel, 1);
    wl->addWidget(m_execBtn);
    wl->addWidget(m_upgradeBtn);

    auto *tasksPage = new QWidget(this);
    auto *tpl = new QVBoxLayout(tasksPage);
    tpl->setContentsMargins(0, 0, 0, 0);
    tpl->setSpacing(0);
    tpl->addWidget(filterRow);
    tpl->addWidget(m_stack, 1);
    tpl->addWidget(m_detail);
    m_buildView = new GradleBuildView(this);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(tasksPage);
    m_pages->addWidget(m_buildView);
    m_tabs = new QTabBar(this);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->addTab(tr("Tasks"));
    m_tabs->addTab(tr("Build"));
    connect(m_tabs, &QTabBar::currentChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i == 1)
            m_buildView->refresh();
    });
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(8, 4, 8, 0);
    tabRow->addWidget(m_tabs);
    tabRow->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_folder);
    layout->addWidget(m_wrapperRow);
    layout->addLayout(tabRow);
    layout->addWidget(m_pages, 1);

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
        Platform::killTree(m_proc); // Windows: gradlew.bat runs in cmd
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_tree->clear();
    m_folder->setText(dir.isEmpty() ? QString() : QDir::toNativeSeparators(QDir::home().relativeFilePath(dir)).prepend(QStringLiteral("~/")));
    m_buildView->setRoot(dir);
    updateDetail();
    updateWrapper();
}

void GradlePanel::showBuild()
{
    m_tabs->setCurrentIndex(1);
}

void GradlePanel::updateWrapper()
{
    const JvmTools::Found v = m_dir.isEmpty() ? JvmTools::Found() : JvmTools::wrapperVersion(m_dir);
    const bool exec = !m_dir.isEmpty() && JvmTools::wrapperNeedsExecBit(m_dir);
    m_wrapperRow->setVisible(!v.isEmpty() || exec);
    m_wrapperLabel->setText(v.isEmpty() ? tr("Gradle wrapper") : tr("Gradle %1 (wrapper)").arg(v.value));
    m_upgradeBtn->setVisible(!v.isEmpty() && !exec);
    m_execBtn->setVisible(exec);
}

void GradlePanel::upgradeWrapper()
{
    const JvmTools::Found current = JvmTools::wrapperVersion(m_dir);
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://services.gradle.org/versions/current")));
    req.setTransferTimeout(15000);
    m_upgradeBtn->setEnabled(false);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, current] {
        reply->deleteLater();
        m_upgradeBtn->setEnabled(true);
        const QString latest = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("version")).toString();
        if (reply->error() != QNetworkReply::NoError || latest.isEmpty()) {
            QMessageBox::warning(this, tr("Upgrade Gradle Wrapper"), tr("Could not read the current Gradle release from services.gradle.org: %1").arg(reply->errorString()));
            return;
        }
        if (latest == current.value) {
            QMessageBox::information(this, tr("Upgrade Gradle Wrapper"), tr("The wrapper already uses Gradle %1, the current release.").arg(latest));
            return;
        }
        const bool major = current.value.section(QLatin1Char('.'), 0, 0) != latest.section(QLatin1Char('.'), 0, 0);
        const auto answer = QMessageBox::question(
            this, tr("Upgrade Gradle Wrapper"),
            tr("Upgrade the wrapper from Gradle %1 to %2?\n\nQODE sets the new version in gradle/wrapper/gradle-wrapper.properties, "
               "then runs, in the terminal:\n%3 wrapper --gradle-version %2\nso the wrapper scripts and jar come from the new "
               "version too.%4")
                .arg(current.value, latest, GradleTasks::runner(m_dir),
                     major ? tr("\n\nThis is a major upgrade: plugins of the build (Kotlin, Android, Compose …) may need newer "
                                "versions too. Check Gradle's upgrade guide if the build fails afterwards.")
                           : QString()));
        if (answer != QMessageBox::Yes)
            return;
        // The properties first: the wrapper task runs the Gradle they name, and the old one may not start at all (Gradle
        // 8.7 on JDK 25).
        QString error;
        if (!JvmTools::setWrapperVersion(m_dir, latest, &error)) {
            QMessageBox::warning(this, tr("Upgrade Gradle Wrapper"), error);
            return;
        }
        updateWrapper();
        emit wrapperChanged();
        emit runRequested(QStringLiteral("%1 wrapper --gradle-version %2 --distribution-type bin").arg(GradleTasks::runner(m_dir), latest), m_dir);
    });
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
        Platform::killTree(m_proc); // Windows: gradlew.bat runs in cmd
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
    const QString wrapper = Platform::wrapperFile(m_dir, QStringLiteral("gradlew"));
    Platform::setCommand(proc, wrapper.isEmpty() ? QStringLiteral("gradle") : wrapper, args);
    proc->start();
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
