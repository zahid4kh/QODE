#include "ExpoBar.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QActionGroup>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QToolButton>

namespace {
const QString kDefault = QStringLiteral("start");

QString quoted(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}
} // namespace

ExpoBar::ExpoBar(QWidget *parent)
    : QWidget(parent), m_watcher(new QFileSystemWatcher(this)), m_redetect(new QTimer(this)), m_menu(new QMenu(this))
{
    m_redetect->setSingleShot(true);
    m_redetect->setInterval(600);
    connect(m_redetect, &QTimer::timeout, this, &ExpoBar::redetect);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_redetect, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::fileChanged, m_redetect, qOverload<>(&QTimer::start));

    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(4, 0, 4, 0);
    l->setSpacing(2);
    m_run = new QToolButton(this);
    m_run->setAutoRaise(true);
    m_run->setIconSize(QSize(16, 16));
    m_run->setPopupMode(QToolButton::MenuButtonPopup);
    m_run->setMenu(m_menu);
    Icons::bind(m_run, QStringLiteral(":/new-icons/play.svg"));
    m_app = new QLabel(this);
    m_app->setContentsMargins(2, 0, 6, 0);
    m_config = new QToolButton(this);
    m_config->setAutoRaise(true);
    m_config->setIconSize(QSize(16, 16));
    Icons::bind(m_config, QStringLiteral(":/new-icons/file-json.svg"));
    l->addWidget(m_run);
    l->addWidget(m_app);
    l->addWidget(m_config);

    connect(m_run, &QToolButton::clicked, this, &ExpoBar::runLast);
    connect(m_config, &QToolButton::clicked, this, &ExpoBar::configureRequested);
    connect(m_menu, &QMenu::aboutToShow, this, &ExpoBar::rebuildMenu);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &ExpoBar::refresh);
    setVisible(false);
}

const ExpoApp *ExpoBar::currentApp() const
{
    if (m_apps.isEmpty())
        return nullptr;
    const QString rel = SettingsManager::instance().expo().value(QStringLiteral("dir")).toString();
    for (const ExpoApp &a : m_apps)
        if (a.rel == rel)
            return &a;
    return &m_apps.first();
}

QStringList ExpoBar::appDirs() const
{
    QStringList out;
    for (const ExpoApp &a : m_apps)
        out << a.dir;
    return out;
}

const ExpoApp *ExpoBar::appForFile(const QString &path) const
{
    const ExpoApp *best = nullptr;
    for (const ExpoApp &a : m_apps)
        if ((path == a.dir || path.startsWith(a.dir + QLatin1Char('/'))) && (!best || a.dir.size() > best->dir.size()))
            best = &a;
    return best;
}

void ExpoBar::setProjectRoot(const QString &root)
{
    m_root = root;
    redetect();
}

void ExpoBar::redetect()
{
    const QList<ExpoApp> found = ExpoProject::detect(m_root);
    const bool was = isAvailable();
    // Changed anything that matters to the UI or the schema?
    bool same = found.size() == m_apps.size();
    for (int i = 0; same && i < found.size(); ++i)
        same = found[i].dir == m_apps[i].dir && found[i].sdk == m_apps[i].sdk && found[i].appJson == m_apps[i].appJson &&
               found[i].name == m_apps[i].name;
    m_apps = found;

    const QStringList watched = m_watcher->files() + m_watcher->directories();
    if (!watched.isEmpty())
        m_watcher->removePaths(watched);
    if (!m_root.isEmpty()) {
        m_watcher->addPath(m_root);
        for (const ExpoApp &a : std::as_const(m_apps)) {
            m_watcher->addPath(a.dir);
            for (const char *f : {"package.json", "app.json", "app.config.json"})
                if (QFileInfo::exists(a.dir + QLatin1Char('/') + QLatin1String(f)))
                    m_watcher->addPath(a.dir + QLatin1Char('/') + QLatin1String(f));
        }
    }
    refresh();
    if (!same || was != isAvailable())
        emit availabilityChanged();
}

void ExpoBar::refresh()
{
    setVisible(isAvailable());
    if (!isAvailable())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const ExpoApp *app = currentApp();
    m_app->setVisible(m_apps.size() > 1);
    m_app->setText(app ? app->name : QString());
    m_app->setStyleSheet(QStringLiteral("color: %1;").arg(t.textMuted.name()));
    m_app->setToolTip(app ? tr("Expo app: %1").arg(QDir::toNativeSeparators(app->dir)) : QString());

    QString last = SettingsManager::instance().expo().value(QStringLiteral("last")).toString();
    const QList<Entry> all = entries();
    const Entry *chosen = nullptr;
    for (const Entry &e : all)
        if (e.enabled && e.id == last)
            chosen = &e;
    if (!chosen)
        for (const Entry &e : all)
            if (e.id == kDefault)
                chosen = &e;
    m_run->setToolTip(tr("%1 (F5)\nClick the arrow for Android, iOS, prebuild and DevTools").arg(chosen ? chosen->text : QString()));
    m_config->setToolTip(app && !app->appJson.isEmpty() ? tr("Configure app.json (name, icons, permissions, plugins…)")
                                                        : tr("This app has no app.json to configure"));
    m_config->setEnabled(app && !app->appJson.isEmpty());
}

QList<ExpoBar::Entry> ExpoBar::entries() const
{
    QList<Entry> out;
    out.append({QStringLiteral("android"), tr("Run on Android"), QStringLiteral("npx expo run:android"),
                tr("Builds the native app and installs it on a device or emulator"), true});
#ifdef Q_OS_MACOS
    const bool canIos = true;
#else
    const bool canIos = false;
#endif
    out.append({QStringLiteral("ios"), tr("Run on iOS"), QStringLiteral("npx expo run:ios"),
                canIos ? tr("Builds the native app and runs it in the iOS simulator") : tr("Building for iOS needs Xcode, which only runs on macOS"),
                canIos});
    out.append({QStringLiteral("start"), tr("Start Metro / Expo Go"), QStringLiteral("npx expo start"),
                tr("Starts the Metro bundler; scan the QR code with Expo Go or press a / i / w"), true});
    out.append({QStringLiteral("prebuild-android"), tr("Prebuild Android"), QStringLiteral("npx expo prebuild --platform android"),
                tr("Generates the android/ folder from app.json and the config plugins"), true});
    out.append({QStringLiteral("prebuild-ios"), tr("Prebuild iOS"), QStringLiteral("npx expo prebuild --platform ios"),
                tr("Generates the ios/ folder from app.json and the config plugins"), true});
    return out;
}

void ExpoBar::rebuildMenu()
{
    m_menu->clear();
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const ExpoApp *app = currentApp();
    if (!app)
        return;
    auto *head = m_menu->addAction(tr("Expo  ·  %1").arg(app->name));
    head->setEnabled(false);
    m_menu->addSeparator();

    const QString last = SettingsManager::instance().expo().value(QStringLiteral("last")).toString();
    const QList<Entry> all = entries();
    auto add = [&](const Entry &e) {
        QAction *a = m_menu->addAction(e.text);
        a->setIcon(Icons::tinted(e.id == QLatin1String("start") ? QStringLiteral(":/new-icons/terminal.svg")
                                 : e.id.startsWith(QLatin1String("prebuild")) ? QStringLiteral(":/new-icons/package.svg")
                                                                              : QStringLiteral(":/new-icons/play.svg"),
                                 t.editorFg));
        a->setToolTip(e.tip);
        a->setStatusTip(e.command);
        a->setEnabled(e.enabled);
        if (!e.enabled)
            a->setText(e.text + tr("  (not available here)"));
        if (e.id == last)
            a->setText(a->text() + QStringLiteral("  ✓"));
        connect(a, &QAction::triggered, this, [this, id = e.id] { runId(id); });
    };
    for (const Entry &e : all)
        if (e.id == QLatin1String("android") || e.id == QLatin1String("ios") || e.id == QLatin1String("start"))
            add(e);
    m_menu->addSeparator();
    for (const Entry &e : all)
        if (e.id.startsWith(QLatin1String("prebuild")))
            add(e);
    m_menu->addSeparator();
    QAction *dev = m_menu->addAction(Icons::tinted(QStringLiteral(":/new-icons/eye.svg"), t.editorFg), tr("Open React Native DevTools"));
    dev->setToolTip(tr("Opens the debugger of the running Metro session (what pressing j in the Expo CLI does)"));
    connect(dev, &QAction::triggered, this, &ExpoBar::devToolsRequested);
    m_menu->addSeparator();

    if (m_apps.size() > 1) {
        QMenu *apps = m_menu->addMenu(tr("App: %1").arg(app->name));
        auto *group = new QActionGroup(apps);
        for (const ExpoApp &a : std::as_const(m_apps)) {
            QAction *act = apps->addAction(a.rel.isEmpty() ? a.name : QStringLiteral("%1  (%2)").arg(a.name, a.rel));
            act->setCheckable(true);
            act->setChecked(a.dir == app->dir);
            group->addAction(act);
            connect(act, &QAction::triggered, this, [this, dir = a.dir] { useApp(dir); });
        }
    }
    QAction *cfg = m_menu->addAction(Icons::tinted(QStringLiteral(":/new-icons/file-json.svg"), t.editorFg), tr("Configure app.json…"));
    cfg->setEnabled(!app->appJson.isEmpty());
    connect(cfg, &QAction::triggered, this, &ExpoBar::configureRequested);
    QAction *cache = m_menu->addAction(Icons::tinted(QStringLiteral(":/new-icons/trash-2.svg"), t.editorFg), tr("Expo Schema Cache…"));
    cache->setToolTip(tr("The app.json schema QODE downloaded for completion and validation"));
    connect(cache, &QAction::triggered, this, &ExpoBar::schemaCacheRequested);
}

QString ExpoBar::shellCommand(const QString &command) const
{
    const ExpoApp *app = currentApp();
    if (!app)
        return command;
    // A subshell with the absolute folder: the terminal may have been moved elsewhere, and its own directory stays put.
    return QStringLiteral("(cd %1 && %2)").arg(quoted(app->dir), command);
}

void ExpoBar::runId(const QString &id)
{
    for (const Entry &e : entries()) {
        if (e.id != id || !e.enabled)
            continue;
        QJsonObject cfg = SettingsManager::instance().expo();
        cfg.insert(QStringLiteral("last"), id);
        SettingsManager::instance().setExpo(cfg);
        refresh();
        emit commandRequested(shellCommand(e.command), id);
        return;
    }
}

void ExpoBar::runLast()
{
    const QString last = SettingsManager::instance().expo().value(QStringLiteral("last")).toString();
    for (const Entry &e : entries())
        if (e.id == last && e.enabled) {
            runId(last);
            return;
        }
    runId(kDefault);
}

void ExpoBar::useApp(const QString &dir)
{
    for (const ExpoApp &a : std::as_const(m_apps))
        if (a.dir == dir) {
            QJsonObject cfg = SettingsManager::instance().expo();
            cfg.insert(QStringLiteral("dir"), a.rel);
            SettingsManager::instance().setExpo(cfg);
            refresh();
            emit appChanged();
            return;
        }
}
