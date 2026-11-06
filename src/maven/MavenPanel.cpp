#include "MavenPanel.h"
#include "platform/Platform.h"
#include "platform/AppPaths.h"

#include "jvm/JvmTools.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSet>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

enum Role {
    RoleGoal = Qt::UserRole + 1, // phase name or prefix:goal; empty on group rows
    RoleOrder,                   // lifecycle index of a phase (runs selected phases in order)
    RoleCoordinate,              // dependency rows: group:artifact:version
    RoleScope,
    RoleDescription,
};

QIcon icon(const char *name, const QColor &c)
{
    return Icons::tinted(QStringLiteral(":/new-icons/%1.svg").arg(QLatin1String(name)), c, c);
}

} // namespace

MavenPanel::MavenPanel(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(tr("MAVEN"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);

    auto button = [this](const QString &iconName, const QString &tip, bool checkable = false) {
        auto *b = new QToolButton(this);
        Icons::bind(b, QStringLiteral(":/new-icons/%1.svg").arg(iconName));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        return b;
    };
    m_goalBtn = button(QStringLiteral("terminal"), tr("Execute Maven goal… (any command line, with the panel's options)"));
    m_effectiveBtn = button(QStringLiteral("file-code"), tr("Show the effective POM (help:effective-pom)"));
    m_reloadBtn = button(QStringLiteral("refresh-cw"), tr("Reload the project: read the poms again, reload plugins and dependencies"));
    auto *closeBtn = button(QStringLiteral("x"), tr("Close Maven panel"));
    connect(m_goalBtn, &QToolButton::clicked, this, &MavenPanel::executeGoal);
    connect(m_effectiveBtn, &QToolButton::clicked, this, [this] { loadEffectivePom(true); });
    connect(m_reloadBtn, &QToolButton::clicked, this, &MavenPanel::reload);
    connect(closeBtn, &QToolButton::clicked, this, &MavenPanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(m_title, 1);
    hl->addWidget(m_goalBtn);
    hl->addWidget(m_effectiveBtn);
    hl->addWidget(m_reloadBtn);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_folder = new QLabel(this);
    m_folder->setObjectName(QStringLiteral("emptyText"));
    m_folder->setContentsMargins(10, 6, 10, 0);
    m_folder->setTextInteractionFlags(Qt::NoTextInteraction);

    // Wrapper / Maven version, and the exec bit a zip download loses.
    m_wrapperRow = new QWidget(this);
    auto *wl = new QHBoxLayout(m_wrapperRow);
    wl->setContentsMargins(10, 4, 6, 0);
    wl->setSpacing(4);
    m_wrapperLabel = new QLabel(m_wrapperRow);
    m_wrapperLabel->setObjectName(QStringLiteral("emptyText"));
    m_wrapperLabel->setWordWrap(true);
    m_execBtn = new QToolButton(m_wrapperRow);
    m_execBtn->setText(tr("Make mvnw executable"));
    m_execBtn->setToolTip(tr("mvnw has lost its exec bit (a zip download, or a checkout without file modes): chmod +x mvnw"));
    m_execBtn->setAutoRaise(true);
    connect(m_execBtn, &QToolButton::clicked, this, [this] {
        QFile f(m_dir + QStringLiteral("/mvnw"));
        if (!f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther))
            QMessageBox::warning(this, tr("Maven"), tr("Could not change the mode of mvnw: %1").arg(f.errorString()));
        updateWrapper();
    });
    wl->addWidget(m_wrapperLabel, 1);
    wl->addWidget(m_execBtn);

    // Options: offline, skip tests, profiles; the module the commands run in.
    m_offlineBtn = new QToolButton(this);
    m_offlineBtn->setText(tr("Offline"));
    m_offlineBtn->setCheckable(true);
    m_offlineBtn->setAutoRaise(true);
    m_offlineBtn->setToolTip(tr("Work offline: use only the local repository (-o)"));
    m_skipTestsBtn = new QToolButton(this);
    m_skipTestsBtn->setText(tr("Skip Tests"));
    m_skipTestsBtn->setCheckable(true);
    m_skipTestsBtn->setAutoRaise(true);
    m_skipTestsBtn->setToolTip(tr("Compile the tests but do not run them (-DskipTests)"));
    m_profilesBtn = new QToolButton(this);
    m_profilesBtn->setAutoRaise(true);
    m_profilesBtn->setPopupMode(QToolButton::InstantPopup);
    m_profilesBtn->setMenu(new QMenu(m_profilesBtn));
    m_profilesBtn->setToolTip(tr("Profiles to activate (-P)"));
    for (QToolButton *b : {m_offlineBtn, m_skipTestsBtn, m_profilesBtn})
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_offlineBtn, &QToolButton::toggled, this, [this](bool on) {
        m_offline = on;
        saveOptions();
    });
    connect(m_skipTestsBtn, &QToolButton::toggled, this, [this](bool on) {
        m_skipTests = on;
        saveOptions();
    });
    auto *optionRow = new QWidget(this);
    auto *ol = new QHBoxLayout(optionRow);
    ol->setContentsMargins(6, 4, 6, 0);
    ol->setSpacing(2);
    ol->addWidget(m_offlineBtn);
    ol->addWidget(m_skipTestsBtn);
    ol->addWidget(m_profilesBtn);
    ol->addStretch(1);

    m_moduleCombo = new QComboBox(this);
    m_moduleCombo->setToolTip(tr("Run in one module (-pl <module> -am builds what it depends on too), or in the whole build"));
    connect(m_moduleCombo, &QComboBox::activated, this, [this] {
        m_module = m_moduleCombo->currentData().toString();
        saveOptions();
        populatePlugins();
        populateDependencies();
    });
    m_moduleRow = new QWidget(this);
    auto *ml = new QHBoxLayout(m_moduleRow);
    ml->setContentsMargins(10, 4, 8, 0);
    ml->setSpacing(6);
    auto *moduleLabel = new QLabel(tr("Module"), m_moduleRow);
    moduleLabel->setObjectName(QStringLiteral("emptyText"));
    ml->addWidget(moduleLabel);
    ml->addWidget(m_moduleCombo, 1);

    m_tabs = new QTabBar(this);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->addTab(tr("Lifecycle"));
    m_tabs->addTab(tr("Plugins"));
    m_tabs->addTab(tr("Dependencies"));
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(8, 6, 8, 0);
    tabRow->addWidget(m_tabs);
    tabRow->addStretch(1);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &MavenPanel::applyFilter);
    auto *filterRow = new QWidget(this);
    auto *fl = new QHBoxLayout(filterRow);
    fl->setContentsMargins(8, 6, 8, 6);
    fl->addWidget(m_filter);

    m_lifecycleTree = makeTree();
    m_pluginTree = makeTree();
    m_depTree = makeTree();
    m_lifecycleTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_lifecycleTree->setRootIsDecorated(false);
    m_pages = new QStackedWidget(this);
    QTreeWidget *trees[3] = {m_lifecycleTree, m_pluginTree, m_depTree};
    for (int i = 0; i < 3; ++i) {
        m_stateTitle[i] = new QLabel(this);
        m_stateTitle[i]->setAlignment(Qt::AlignCenter);
        QFont f = m_stateTitle[i]->font();
        f.setBold(true);
        m_stateTitle[i]->setFont(f);
        m_stateText[i] = new QLabel(this);
        m_stateText[i]->setObjectName(QStringLiteral("emptyText"));
        m_stateText[i]->setAlignment(Qt::AlignCenter);
        m_stateText[i]->setWordWrap(true);
        m_stateText[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_stateRetry[i] = new QToolButton(this);
        m_stateRetry[i]->setText(tr("Try again"));
        m_stateRetry[i]->setToolButtonStyle(Qt::ToolButtonTextOnly);
        auto *state = new QWidget(this);
        auto *sl = new QVBoxLayout(state);
        sl->setContentsMargins(18, 18, 18, 18);
        sl->addStretch(1);
        sl->addWidget(m_stateTitle[i]);
        sl->addWidget(m_stateText[i]);
        sl->addWidget(m_stateRetry[i], 0, Qt::AlignHCenter);
        sl->addStretch(2);
        m_pageStacks[i] = new QStackedWidget(this);
        m_pageStacks[i]->addWidget(trees[i]); // 0
        m_pageStacks[i]->addWidget(state);    // 1
        m_pages->addWidget(m_pageStacks[i]);
    }
    connect(m_stateRetry[Plugins], &QToolButton::clicked, this, [this] { loadEffectivePom(false); });
    connect(m_stateRetry[Dependencies], &QToolButton::clicked, this, &MavenPanel::loadDependencyTree);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        m_pages->setCurrentIndex(i);
        if (i == Plugins && m_effectiveStamp != pomStamp() && !m_effectiveProc && MavenProject::mavenAvailable(m_dir))
            loadEffectivePom(false);
        if (i == Dependencies && m_treeStamp != pomStamp() && !m_treeProc)
            loadDependencyTree();
        applyFilter();
        updateDetail();
    });

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("emptyText"));
    m_detail->setWordWrap(true);
    m_detail->setContentsMargins(10, 6, 10, 8);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setStyleSheet(QStringLiteral("QLabel { border-top: 1px solid palette(shadow); }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_folder);
    layout->addWidget(m_wrapperRow);
    layout->addWidget(optionRow);
    layout->addWidget(m_moduleRow);
    layout->addLayout(tabRow);
    layout->addWidget(filterRow);
    layout->addWidget(m_pages, 1);
    layout->addWidget(m_detail);

    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        populateLifecycle();
        populatePlugins();
        populateDependencies();
    });
    updateDetail();
}

QTreeWidget *MavenPanel::makeTree()
{
    auto *tree = new QTreeWidget(this);
    tree->setHeaderHidden(true);
    tree->setIndentation(14);
    tree->setIconSize(QSize(14, 14));
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->setUniformRowHeights(true);
    tree->setMouseTracking(true);
    tree->installEventFilter(this);
    connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree](const QPoint &pos) { showContextMenu(tree, pos); });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this, tree](QTreeWidgetItem *it) {
        if (tree == m_depTree) {
            // A direct dependency opens its declaration in the pom.
            const QString coord = it->data(0, RoleCoordinate).toString();
            if (!it->parent() || coord.isEmpty())
                return;
            for (const MavenProject::Pom &pom : std::as_const(m_poms))
                for (const MavenProject::Dependency &d : pom.dependencies)
                    if (coord.startsWith(d.groupId + QLatin1Char(':') + d.artifactId + QLatin1Char(':'))) {
                        emit openFileRequested(pom.path, d.line);
                        return;
                    }
            return;
        }
        if (!it->data(0, RoleGoal).toString().isEmpty())
            runItems(tree == m_lifecycleTree ? tree->selectedItems() : QList<QTreeWidgetItem *>{it});
    });
    connect(tree, &QTreeWidget::itemSelectionChanged, this, &MavenPanel::updateDetail);
    return tree;
}

QString MavenPanel::cacheFile(const QString &name) const
{
    const QString dir = AppPaths::cache(QStringLiteral("maven"));
    QDir().mkpath(dir);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(m_dir.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
    return dir + QLatin1Char('/') + name + QLatin1Char('-') + hash + QStringLiteral(".xml");
}

void MavenPanel::killProcess(QProcess *&proc)
{
    if (!proc)
        return;
    proc->disconnect(this);
    proc->kill();
    proc->deleteLater();
    proc = nullptr;
}

void MavenPanel::setBuildDir(const QString &dir)
{
    if (dir == m_dir)
        return;
    m_dir = dir;
    killProcess(m_effectiveProc);
    killProcess(m_treeProc);
    m_poms.clear();
    m_effective.clear();
    m_tree.clear();
    m_pomStamp = m_effectiveStamp = m_treeStamp = -1;
    m_folder->setText(dir.isEmpty() ? QString() : QDir::toNativeSeparators(QDir::home().relativeFilePath(dir)).prepend(QStringLiteral("~/")));
    const QJsonObject o = SettingsManager::instance().mavenOptions();
    m_offline = o.value(QStringLiteral("offline")).toBool();
    m_skipTests = o.value(QStringLiteral("skipTests")).toBool();
    m_profiles.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("profiles")).toArray())
        m_profiles << v.toString();
    m_module = o.value(QStringLiteral("module")).toString();
    const QSignalBlocker b1(m_offlineBtn), b2(m_skipTestsBtn);
    m_offlineBtn->setChecked(m_offline);
    m_skipTestsBtn->setChecked(m_skipTests);
    for (QTreeWidget *t : {m_lifecycleTree, m_pluginTree, m_depTree})
        t->clear();
    updateWrapper();
    if (!dir.isEmpty() && isVisible())
        ensureLoaded();
}

void MavenPanel::saveOptions()
{
    if (m_dir.isEmpty())
        return;
    QJsonObject o;
    if (m_offline)
        o.insert(QStringLiteral("offline"), true);
    if (m_skipTests)
        o.insert(QStringLiteral("skipTests"), true);
    if (!m_profiles.isEmpty())
        o.insert(QStringLiteral("profiles"), QJsonArray::fromStringList(m_profiles));
    if (!m_module.isEmpty())
        o.insert(QStringLiteral("module"), m_module);
    SettingsManager::instance().setMavenOptions(o);
    updateDetail();
}

void MavenPanel::updateWrapper()
{
    if (m_dir.isEmpty()) {
        m_wrapperRow->hide();
        return;
    }
    const QString wrapper = MavenProject::wrapperVersion(m_dir);
    const bool exec = MavenProject::wrapperNeedsExecBit(m_dir);
    const bool available = MavenProject::mavenAvailable(m_dir);
    m_wrapperRow->show();
    m_execBtn->setVisible(exec);
    if (!wrapper.isEmpty())
        m_wrapperLabel->setText(tr("Maven %1 (wrapper)").arg(wrapper));
    else if (available)
        m_wrapperLabel->setText(tr("Maven on PATH (no wrapper)"));
    else
        m_wrapperLabel->setText(tr("⚠ Maven is not installed: there is no mvnw in the project and no mvn on PATH."));
}

qint64 MavenPanel::pomStamp() const
{
    qint64 stamp = 0;
    if (m_dir.isEmpty())
        return stamp;
    stamp = QFileInfo(m_dir + QStringLiteral("/pom.xml")).lastModified().toMSecsSinceEpoch();
    for (const MavenProject::Pom &p : m_poms)
        stamp = qMax(stamp, QFileInfo(p.path).lastModified().toMSecsSinceEpoch());
    return stamp;
}

void MavenPanel::ensureLoaded()
{
    if (m_dir.isEmpty())
        return;
    if (m_poms.isEmpty() || m_pomStamp != pomStamp())
        readPoms();
    if (m_tabs->currentIndex() == Plugins && m_effectiveStamp != pomStamp() && !m_effectiveProc && MavenProject::mavenAvailable(m_dir))
        loadEffectivePom(false);
    if (m_tabs->currentIndex() == Dependencies && m_treeStamp != pomStamp() && !m_treeProc)
        loadDependencyTree();
}

void MavenPanel::reload()
{
    if (m_dir.isEmpty())
        return;
    m_effectiveStamp = m_treeStamp = -1;
    readPoms();
    updateWrapper();
    if (m_tabs->currentIndex() == Plugins && MavenProject::mavenAvailable(m_dir))
        loadEffectivePom(false);
    if (m_tabs->currentIndex() == Dependencies)
        loadDependencyTree();
}

void MavenPanel::readPoms()
{
    m_poms = MavenProject::readModules(m_dir);
    m_pomStamp = pomStamp();
    if (m_poms.isEmpty()) {
        for (int i = 0; i < 3; ++i)
            showPageState(Page(i), tr("pom.xml could not be read"), tr("The pom of %1 is not valid XML.").arg(QDir::toNativeSeparators(m_dir)), false);
        return;
    }
    m_title->setText(tr("MAVEN · %1").arg(m_poms.first().title().toUpper()));
    // Modules: the root build, then each module by its artifactId (what -pl takes as :artifactId).
    m_moduleCombo->clear();
    m_moduleCombo->addItem(tr("Whole build (%1)").arg(m_poms.first().artifactId), QString());
    for (int i = 1; i < m_poms.size(); ++i)
        m_moduleCombo->addItem(m_poms.at(i).artifactId, m_poms.at(i).artifactId);
    const int idx = m_moduleCombo->findData(m_module);
    m_moduleCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    if (idx < 0)
        m_module.clear();
    m_moduleRow->setVisible(m_poms.size() > 1);
    // Profiles: the union over the modules; a stored choice that no longer exists is dropped.
    QStringList ids;
    for (const MavenProject::Pom &p : std::as_const(m_poms))
        for (const MavenProject::Profile &pr : p.profiles)
            if (!ids.contains(pr.id))
                ids << pr.id;
    for (int i = int(m_profiles.size()) - 1; i >= 0; --i)
        if (!ids.contains(m_profiles.at(i)))
            m_profiles.removeAt(i);
    QMenu *menu = m_profilesBtn->menu();
    menu->clear();
    for (const QString &id : std::as_const(ids)) {
        bool byDefault = false;
        for (const MavenProject::Pom &p : std::as_const(m_poms))
            for (const MavenProject::Profile &pr : p.profiles)
                byDefault = byDefault || (pr.id == id && pr.activeByDefault);
        QAction *a = menu->addAction(byDefault ? tr("%1 (active by default)").arg(id) : id);
        a->setCheckable(true);
        a->setChecked(m_profiles.contains(id));
        connect(a, &QAction::toggled, this, [this, id](bool on) {
            if (on && !m_profiles.contains(id))
                m_profiles << id;
            else if (!on)
                m_profiles.removeAll(id);
            saveOptions();
            updateProfilesButton();
        });
    }
    m_profilesBtn->setVisible(!ids.isEmpty());
    updateProfilesButton();
    populateLifecycle();
    populatePlugins();
    if (m_tree.isEmpty() && m_treeStamp < 0)
        showPageState(Dependencies, tr("Dependencies"), tr("Open this tab to resolve the dependency tree (mvn dependency:tree)."), false);
}

void MavenPanel::updateProfilesButton()
{
    m_profilesBtn->setText(m_profiles.isEmpty() ? tr("Profiles ▾") : tr("Profiles: %1 ▾").arg(m_profiles.join(QStringLiteral(", "))));
}

void MavenPanel::showPageState(Page page, const QString &title, const QString &text, bool retry)
{
    m_stateTitle[page]->setText(title);
    m_stateText[page]->setText(text);
    m_stateRetry[page]->setVisible(retry);
    m_pageStacks[page]->setCurrentIndex(1);
}

void MavenPanel::populateLifecycle()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_lifecycleTree->clear();
    int order = 0;
    for (const MavenProject::Phase &p : MavenProject::lifecycle()) {
        auto *it = new QTreeWidgetItem(m_lifecycleTree);
        it->setText(0, p.name);
        it->setData(0, RoleGoal, p.name);
        it->setData(0, RoleOrder, order++);
        it->setData(0, RoleDescription, p.description);
        it->setToolTip(0, p.name + QStringLiteral(" — ") + p.description);
        it->setIcon(0, icon(p.name == QLatin1String("clean") ? "trash-2" : p.name == QLatin1String("test") ? "check"
                            : p.name == QLatin1String("package") || p.name == QLatin1String("install") ? "package"
                            : p.name == QLatin1String("deploy") ? "external-link" : "play",
                            t.textMuted));
    }
    m_pageStacks[Lifecycle]->setCurrentIndex(0);
    applyFilter();
}

void MavenPanel::populatePlugins()
{
    if (m_poms.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    // The module the commands run in decides whose plugins are listed (the root's for the whole build).
    MavenProject::Pom pom = m_poms.first();
    for (const MavenProject::Pom &p : std::as_const(m_poms))
        if (!m_module.isEmpty() && p.artifactId == m_module)
            pom = p;
    QList<MavenProject::Plugin> plugins;
    const bool effective = m_effective.contains(pom.artifactId);
    if (effective) {
        plugins = m_effective.value(pom.artifactId).plugins;
    } else {
        plugins = MavenProject::defaultPlugins(pom.packaging);
        // Without Maven's view: the default bindings, then what the root pom declares (modules inherit it), then the
        // module's own.
        QList<MavenProject::Plugin> declared = pom.plugins;
        if (pom.path != m_poms.first().path)
            declared = m_poms.first().plugins + declared;
        for (const MavenProject::Plugin &p : std::as_const(declared)) {
            bool merged = false;
            for (MavenProject::Plugin &d : plugins)
                if (d.artifactId == p.artifactId) {
                    const QString version = d.version;
                    d = p;
                    if (d.version.isEmpty())
                        d.version = version;
                    merged = true;
                }
            if (!merged)
                plugins << p;
        }
    }
    std::sort(plugins.begin(), plugins.end(), [](const MavenProject::Plugin &a, const MavenProject::Plugin &b) { return a.prefix() < b.prefix(); });
    QSet<QString> expanded;
    for (int i = 0; i < m_pluginTree->topLevelItemCount(); ++i)
        if (m_pluginTree->topLevelItem(i)->isExpanded())
            expanded << m_pluginTree->topLevelItem(i)->text(0);
    m_pluginTree->clear();
    QFont bold = m_pluginTree->font();
    bold.setBold(true);
    for (const MavenProject::Plugin &p : std::as_const(plugins)) {
        auto *top = new QTreeWidgetItem(m_pluginTree);
        const QString prefix = p.prefix();
        top->setText(0, p.version.isEmpty() ? prefix : QStringLiteral("%1  (%2)").arg(prefix, p.version));
        top->setFont(0, bold);
        top->setIcon(0, icon("package", t.accent));
        top->setToolTip(0, QStringLiteral("%1:%2%3").arg(p.groupId, p.artifactId, p.version.isEmpty() ? QString() : QLatin1Char(':') + p.version));
        top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        QList<MavenProject::Goal> goals = MavenProject::pluginGoals(p);
        if (goals.isEmpty())
            goals = MavenProject::knownGoals(p.artifactId);
        for (const MavenProject::Goal &g : std::as_const(goals)) {
            auto *it = new QTreeWidgetItem(top);
            const QString goal = prefix + QLatin1Char(':') + g.name;
            it->setText(0, goal);
            it->setData(0, RoleGoal, goal);
            QString tip = g.description;
            if (!g.phase.isEmpty())
                tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Bound to the %1 phase by default").arg(g.phase);
            it->setData(0, RoleDescription, tip);
            it->setToolTip(0, tip.isEmpty() ? goal : goal + QStringLiteral(" — ") + tip);
            it->setIcon(0, icon("play", t.textMuted));
        }
        if (goals.isEmpty()) {
            auto *it = new QTreeWidgetItem(top);
            it->setText(0, tr("goals load once Maven has downloaded the plugin"));
            it->setForeground(0, t.textMuted);
            it->setFlags(Qt::ItemIsEnabled);
        }
        top->setExpanded(expanded.contains(top->text(0)));
    }
    m_pageStacks[Plugins]->setCurrentIndex(0);
    applyFilter();
}

void MavenPanel::populateDependencies()
{
    if (m_tree.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_depTree->clear();
    QFont bold = m_depTree->font();
    bold.setBold(true);
    for (const MavenProject::TreeModule &m : std::as_const(m_tree)) {
        if (!m_module.isEmpty() && m.name != m_module && m_tree.size() > 1)
            continue;
        auto *top = new QTreeWidgetItem(m_depTree);
        top->setText(0, m.name);
        top->setFont(0, bold);
        top->setIcon(0, icon("apachemaven", t.accent));
        top->setToolTip(0, m.coordinate);
        top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        QList<QTreeWidgetItem *> parents{top};
        for (const MavenProject::TreeNode &n : m.nodes) {
            while (parents.size() > n.depth + 1)
                parents.removeLast();
            auto *it = new QTreeWidgetItem(parents.last());
            const QString scope = n.scope.isEmpty() || n.scope == QLatin1String("compile") ? QString() : QStringLiteral("  [%1]").arg(n.scope);
            it->setText(0, QStringLiteral("%1:%2  %3%4").arg(n.groupId, n.artifactId, n.version, scope));
            it->setData(0, RoleCoordinate, n.coordinate());
            it->setData(0, RoleScope, n.scope);
            QString tip = n.coordinate();
            if (!n.scope.isEmpty())
                tip += tr("\nscope: %1").arg(n.scope);
            if (!n.note.isEmpty())
                tip += QLatin1Char('\n') + n.note;
            it->setToolTip(0, tip);
            const bool test = n.scope == QLatin1String("test") || n.scope == QLatin1String("provided");
            it->setIcon(0, icon(n.depth == 0 ? "package" : "file", test ? t.textMuted : n.depth == 0 ? t.accent : t.editorFg));
            if (test || n.optional)
                it->setForeground(0, t.textMuted);
            while (parents.size() > n.depth + 1)
                parents.removeLast();
            parents << it;
        }
        top->setExpanded(true);
    }
    m_pageStacks[Dependencies]->setCurrentIndex(0);
    applyFilter();
}

void MavenPanel::loadEffectivePom(bool open)
{
    if (m_dir.isEmpty())
        return;
    if (!MavenProject::mavenAvailable(m_dir)) {
        QMessageBox::information(this, tr("Effective POM"), tr("Maven is not installed: there is no mvnw in the project and no mvn on PATH."));
        return;
    }
    m_openEffective = m_openEffective || open;
    if (m_effectiveProc)
        return;
    const QString out = cacheFile(QStringLiteral("effective-pom"));
    if (open && m_effectiveStamp == pomStamp() && QFileInfo::exists(out)) {
        m_openEffective = false;
        emit openFileRequested(out, 0);
        return;
    }
    if (m_effective.isEmpty())
        showPageState(Plugins, tr("Reading the effective POM…"), tr("Maven resolves the parent poms and plugin versions. The first run downloads what it needs."), false);
    m_effectiveBtn->setEnabled(false);
    m_effectiveProc = new QProcess(this);
    m_effectiveProc->setWorkingDirectory(m_dir);
    QProcess *proc = m_effectiveProc;
    connect(proc, &QProcess::finished, this, [this, proc, out](int code, QProcess::ExitStatus) {
        const QString log = QString::fromUtf8(proc->readAllStandardOutput()) + QString::fromUtf8(proc->readAllStandardError());
        proc->deleteLater();
        m_effectiveProc = nullptr;
        m_effectiveBtn->setEnabled(true);
        QFile f(out);
        if (code != 0 || !f.open(QIODevice::ReadOnly)) {
            QStringList errors;
            for (const QString &l : log.split(QLatin1Char('\n')))
                if (l.startsWith(QLatin1String("[ERROR]")))
                    errors << l.mid(8);
            if (m_effective.isEmpty())
                populatePlugins(); // the pom's own plugins
            QMessageBox::warning(this, tr("Effective POM"), tr("Maven could not compute the effective POM:\n\n%1")
                                                                .arg(errors.mid(0, 6).join(QLatin1Char('\n')).left(1200)));
            m_openEffective = false;
            return;
        }
        m_effective = MavenProject::parseEffectivePom(QString::fromUtf8(f.readAll()));
        m_effectiveStamp = pomStamp();
        populatePlugins();
        if (m_openEffective)
            emit openFileRequested(out, 0);
        m_openEffective = false;
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        proc->deleteLater();
        m_effectiveProc = nullptr;
        m_effectiveBtn->setEnabled(true);
        populatePlugins();
    });
    QStringList args = {QStringLiteral("-B"), QStringLiteral("-q"), QStringLiteral("help:effective-pom"), QStringLiteral("-Doutput=") + out};
    if (m_offline)
        args.prepend(QStringLiteral("-o"));
    if (!m_profiles.isEmpty())
        args << QStringLiteral("-P") + m_profiles.join(QLatin1Char(','));
    Platform::setCommand(proc, MavenProject::runnerFile(m_dir), args);
    proc->start();
}

void MavenPanel::loadDependencyTree()
{
    if (m_dir.isEmpty() || m_treeProc)
        return;
    if (!MavenProject::mavenAvailable(m_dir)) {
        showPageState(Dependencies, tr("Maven is not installed"), tr("The dependency tree needs Maven: add the Maven wrapper (mvnw) to the project or install mvn."), true);
        return;
    }
    showPageState(Dependencies, tr("Resolving dependencies…"), tr("mvn dependency:tree. The first run downloads what the build needs."), false);
    m_treeProc = new QProcess(this);
    m_treeProc->setWorkingDirectory(m_dir);
    m_treeProc->setProcessChannelMode(QProcess::MergedChannels);
    QProcess *proc = m_treeProc;
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(proc->readAll());
        proc->deleteLater();
        m_treeProc = nullptr;
        m_tree = MavenProject::parseDependencyTree(out);
        m_treeStamp = pomStamp();
        if (code != 0 && m_tree.isEmpty()) {
            QStringList errors;
            for (const QString &l : out.split(QLatin1Char('\n')))
                if (l.startsWith(QLatin1String("[ERROR]")) && l.size() > 8)
                    errors << l.mid(8);
            showPageState(Dependencies, tr("Maven could not resolve the dependencies"), errors.mid(0, 6).join(QLatin1Char('\n')).left(1200), true);
            return;
        }
        if (m_tree.isEmpty()) {
            showPageState(Dependencies, tr("No dependencies"), tr("This build declares no dependencies."), true);
            return;
        }
        populateDependencies();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        proc->deleteLater();
        m_treeProc = nullptr;
        showPageState(Dependencies, tr("Could not start Maven"), tr("The project has no executable mvnw and mvn is not on PATH."), true);
    });
    QStringList args = {QStringLiteral("-B"), QStringLiteral("dependency:tree")};
    if (m_offline)
        args.prepend(QStringLiteral("-o"));
    if (!m_profiles.isEmpty())
        args << QStringLiteral("-P") + m_profiles.join(QLatin1Char(','));
    Platform::setCommand(proc, MavenProject::runnerFile(m_dir), args);
    proc->start();
}

QTreeWidget *MavenPanel::currentTree() const
{
    switch (m_tabs->currentIndex()) {
    case Plugins: return m_pluginTree;
    case Dependencies: return m_depTree;
    default: return m_lifecycleTree;
    }
}

void MavenPanel::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    QTreeWidget *tree = currentTree();
    // A row shows when it or a row below it matches; matching rows open their parents.
    std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) {
        bool any = false;
        for (int i = 0; i < it->childCount(); ++i)
            any = visit(it->child(i)) || any;
        const bool self = needle.isEmpty() || it->text(0).contains(needle, Qt::CaseInsensitive);
        it->setHidden(!(self || any));
        if (!needle.isEmpty() && any)
            it->setExpanded(true);
        return self || any;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        visit(tree->topLevelItem(i));
}

QString MavenPanel::moduleArgs() const
{
    return m_module.isEmpty() ? QString() : QStringLiteral(" -pl :%1 -am").arg(m_module);
}

QString MavenPanel::optionArgs() const
{
    QString args;
    if (m_offline)
        args += QStringLiteral(" -o");
    if (m_skipTests)
        args += QStringLiteral(" -DskipTests");
    if (!m_profiles.isEmpty())
        args += QStringLiteral(" -P") + m_profiles.join(QLatin1Char(','));
    return args;
}

QString MavenPanel::commandFor(const QStringList &goals, const QString &extraArgs) const
{
    if (goals.isEmpty())
        return {};
    QString cmd = MavenProject::runner(m_dir) + optionArgs() + moduleArgs() + QLatin1Char(' ') + goals.join(QLatin1Char(' '));
    if (!extraArgs.trimmed().isEmpty())
        cmd += QLatin1Char(' ') + extraArgs.trimmed();
    return cmd;
}

QStringList MavenPanel::goalsOf(const QList<QTreeWidgetItem *> &items) const
{
    QList<QTreeWidgetItem *> sorted;
    for (QTreeWidgetItem *it : items)
        if (it && !it->data(0, RoleGoal).toString().isEmpty())
            sorted << it;
    std::stable_sort(sorted.begin(), sorted.end(), [](QTreeWidgetItem *a, QTreeWidgetItem *b) {
        return a->data(0, RoleOrder).toInt() < b->data(0, RoleOrder).toInt();
    });
    QStringList goals;
    for (QTreeWidgetItem *it : std::as_const(sorted))
        goals << it->data(0, RoleGoal).toString();
    return goals;
}

void MavenPanel::runItems(const QList<QTreeWidgetItem *> &items, const QString &extraArgs)
{
    const QString cmd = commandFor(goalsOf(items), extraArgs);
    if (!cmd.isEmpty())
        emit runRequested(cmd, m_dir);
}

void MavenPanel::executeGoal()
{
    if (m_dir.isEmpty())
        return;
    bool ok = false;
    const QString line = QInputDialog::getText(this, tr("Execute Maven Goal"),
                                               tr("Goals and options (the panel's offline / skip tests / profiles / module are added):\n%1 …")
                                                   .arg(MavenProject::runner(m_dir) + optionArgs() + moduleArgs()),
                                               QLineEdit::Normal, QStringLiteral("clean install"), &ok);
    if (!ok || line.trimmed().isEmpty())
        return;
    QString goals = line.trimmed();
    for (const QString &r : {QStringLiteral("./mvnw "), QStringLiteral(".\\mvnw "), QStringLiteral("mvn ")})
        if (goals.startsWith(r))
            goals = goals.mid(r.size());
    emit runRequested(commandFor({goals}), m_dir);
}

void MavenPanel::updateDetail()
{
    QTreeWidget *tree = currentTree();
    const QList<QTreeWidgetItem *> sel = tree->selectedItems();
    if (tree == m_depTree) {
        const QString coord = sel.isEmpty() ? QString() : sel.first()->data(0, RoleCoordinate).toString();
        m_detail->setText(coord.isEmpty() ? tr("Double-click a direct dependency to open its declaration in the pom.")
                                          : sel.first()->toolTip(0));
        return;
    }
    const QStringList goals = goalsOf(sel);
    if (goals.isEmpty()) {
        m_detail->setText(tree == m_lifecycleTree ? tr("Double-click a phase to run it (Ctrl+click several: they run together, in order).")
                                                  : tr("Double-click a goal to run it in the terminal."));
        return;
    }
    const QString description = sel.size() == 1 ? sel.first()->data(0, RoleDescription).toString() : QString();
    m_detail->setText(commandFor(goals) + (description.isEmpty() ? QString() : QLatin1Char('\n') + description));
}

void MavenPanel::showContextMenu(QTreeWidget *tree, const QPoint &pos)
{
    QTreeWidgetItem *it = tree->itemAt(pos);
    if (!it)
        return;
    if (!it->isSelected()) {
        tree->clearSelection();
        tree->setCurrentItem(it);
    }
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    auto ic = [&](const char *n) { return Icons::tinted(QStringLiteral(":/new-icons/%1.svg").arg(QLatin1String(n)), t.editorFg, t.textMuted); };
    QMenu menu(this);
    if (tree == m_depTree) {
        const QString coord = it->data(0, RoleCoordinate).toString();
        if (coord.isEmpty())
            return;
        const QStringList parts = coord.split(QLatin1Char(':'));
        menu.addAction(ic("copy"), tr("Copy Coordinates"), this, [coord] { QApplication::clipboard()->setText(coord); });
        menu.addAction(ic("copy"), tr("Copy as <dependency>"), this, [parts, it] {
            const QString scope = it->data(0, RoleScope).toString();
            QApplication::clipboard()->setText(QStringLiteral("<dependency>\n    <groupId>%1</groupId>\n    <artifactId>%2</artifactId>\n    <version>%3</version>\n%4</dependency>")
                                                   .arg(parts.value(0), parts.value(1), parts.value(2),
                                                        scope.isEmpty() || scope == QLatin1String("compile") ? QString() : QStringLiteral("    <scope>%1</scope>\n").arg(scope)));
        });
        menu.addAction(ic("external-link"), tr("Open on Maven Central"), this, [parts] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://central.sonatype.com/artifact/%1/%2/%3").arg(parts.value(0), parts.value(1), parts.value(2))));
        });
        menu.exec(tree->viewport()->mapToGlobal(pos));
        return;
    }
    const QList<QTreeWidgetItem *> items = tree == m_lifecycleTree ? tree->selectedItems() : QList<QTreeWidgetItem *>{it};
    const QStringList goals = goalsOf(items);
    if (goals.isEmpty())
        return;
    menu.addAction(ic("play"), goals.size() > 1 ? tr("Run '%1'").arg(goals.join(QLatin1Char(' '))) : tr("Run"), this, [this, items] { runItems(items); });
    menu.addAction(ic("pencil"), tr("Run with Arguments…"), this, [this, items, goals] {
        bool ok = false;
        const QString args = QInputDialog::getText(this, tr("Run with Arguments"), tr("Arguments for %1 (for example -X, -U or -Dkey=value):").arg(goals.join(QLatin1Char(' '))),
                                                   QLineEdit::Normal, QString(), &ok);
        if (ok)
            runItems(items, args);
    });
    if (tree == m_lifecycleTree && goals != QStringList{QStringLiteral("clean")} && !goals.contains(QStringLiteral("clean")))
        menu.addAction(ic("trash-2"), tr("Run 'clean %1'").arg(goals.join(QLatin1Char(' '))), this, [this, goals] {
            emit runRequested(commandFor(QStringList{QStringLiteral("clean")} + goals), m_dir);
        });
    menu.addSeparator();
    menu.addAction(ic("copy"), tr("Copy Command"), this, [this, goals] { QApplication::clipboard()->setText(commandFor(goals)); });
    menu.exec(tree->viewport()->mapToGlobal(pos));
}

bool MavenPanel::eventFilter(QObject *watched, QEvent *event)
{
    auto *tree = qobject_cast<QTreeWidget *>(watched);
    if (tree && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && tree != m_depTree && !tree->selectedItems().isEmpty()) {
            runItems(tree == m_lifecycleTree ? tree->selectedItems() : QList<QTreeWidgetItem *>{tree->currentItem()});
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
