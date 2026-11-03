#include "ktor/KtorPanel.h"

#include "ktor/KtorBar.h"
#include "ktor/KtorReloader.h"
#include "ktor/KtorRoutesView.h"
#include "ktor/KtorServer.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "spring/HttpClientView.h"
#include "spring/SpringLogView.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {

QString now()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
}

QString duration(qint64 ms)
{
    const qint64 s = ms / 1000;
    if (s < 60)
        return QStringLiteral("%1s").arg(s);
    if (s < 3600)
        return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1h %2m").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0'));
}

} // namespace

KtorPanel::KtorPanel(QWidget *parent)
    : QWidget(parent), m_uptime(new QTimer(this)), m_filterTimer(new QTimer(this)), m_rescanTimer(new QTimer(this))
{
    auto button = [this](const QString &tip, bool checkable = false) {
        auto *b = new QToolButton(this);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        return b;
    };

    m_logo = new QLabel(this);
    m_logo->setFixedSize(16, 16);
    m_logo->setScaledContents(true);
    m_title = new QLabel(tr("KTOR"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_toggle = button(QString());
    m_restart = button(tr("Restart the server"));
    m_config = button(tr("Port, development mode and arguments"));
    m_closeBtn = button(tr("Close Ktor panel"));
    Icons::bind(m_restart, QStringLiteral(":/new-icons/refresh-cw.svg"));
    Icons::bind(m_config, QStringLiteral(":/new-icons/cog.svg"));
    Icons::bind(m_closeBtn, QStringLiteral(":/new-icons/x.svg"));
    connect(m_closeBtn, &QToolButton::clicked, this, &KtorPanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 4, 0);
    hl->setSpacing(4);
    hl->addWidget(m_logo);
    hl->addWidget(m_title, 1);
    hl->addWidget(m_toggle);
    hl->addWidget(m_restart);
    hl->addWidget(m_config);
    hl->addWidget(m_closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_dot = new QLabel(this);
    m_dot->setFixedSize(9, 9);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_status->setOpenExternalLinks(false);
    connect(m_status, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("emptyText"));
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *statusRow = new QWidget(this);
    auto *sl = new QHBoxLayout(statusRow);
    sl->setContentsMargins(10, 0, 10, 0);
    sl->setSpacing(6);
    sl->addWidget(m_dot, 0, Qt::AlignVCenter);
    sl->addWidget(m_status, 1);

    // --- Log
    m_level = new QComboBox(this);
    m_level->addItem(tr("All levels"), int(SpringLogView::Trace));
    m_level->addItem(tr("Debug +"), int(SpringLogView::Debug));
    m_level->addItem(tr("Info +"), int(SpringLogView::Info));
    m_level->addItem(tr("Warnings +"), int(SpringLogView::Warn));
    m_level->addItem(tr("Errors"), int(SpringLogView::Error));
    m_level->setToolTip(tr("Show only entries of this level and above"));
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter log"));
    m_filter->setClearButtonEnabled(true);
    m_build = button(tr("Show Gradle / Maven build output"), true);
    m_build->setChecked(true);
    m_pause = button(tr("Pause the log (output is kept and shown again when you resume)"), true);
    m_clear = button(tr("Clear the log"));
    m_copy = button(tr("Copy the log (what the filters show)"));
    m_end = button(tr("Jump to the latest entry"));
    Icons::bind(m_build, QStringLiteral(":/new-icons/gradle.svg"));
    Icons::bind(m_clear, QStringLiteral(":/new-icons/trash-2.svg"));
    Icons::bind(m_copy, QStringLiteral(":/new-icons/copy.svg"));
    Icons::bind(m_end, QStringLiteral(":/new-icons/arrow-down.svg"));
    auto *tools = new QWidget(this);
    auto *tl = new QHBoxLayout(tools);
    tl->setContentsMargins(8, 4, 6, 4);
    tl->setSpacing(2);
    tl->addWidget(m_level);
    tl->addWidget(m_filter, 1);
    tl->addWidget(m_build);
    tl->addWidget(m_pause);
    tl->addWidget(m_clear);
    tl->addWidget(m_copy);
    tl->addWidget(m_end);
    m_log = new SpringLogView(this);
    m_log->setPlaceholderText(tr("Start the server to see its output here."));
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("emptyText"));
    m_count->setContentsMargins(10, 3, 10, 4);
    m_count->setStyleSheet(QStringLiteral("QLabel { border-top: 1px solid palette(shadow); }"));
    auto *logPage = new QWidget(this);
    auto *lp = new QVBoxLayout(logPage);
    lp->setContentsMargins(0, 0, 0, 0);
    lp->setSpacing(0);
    lp->addWidget(tools);
    lp->addWidget(m_log, 1);
    lp->addWidget(m_count);

    // --- Routes, Requests, Config
    m_routes = new KtorRoutesView(this);
    m_http = new HttpClientView(this);
    m_http->setScanTexts(tr("Scanning routes…"), tr("No routes found. get(\"/\") { } and the other routing functions show up here."),
                         tr("Scan the routing code again"));
    m_routes->setFormatters([this](const SpringEndpoint &e) { return m_http->endpointAsHttp(e); },
                            [this](const SpringEndpoint &e) { return m_http->endpointAsCurl(e); });
    m_configView = new KtorConfigView(this);

    auto *tabs = new QWidget(this);
    auto *tabsLayout = new QHBoxLayout(tabs);
    tabsLayout->setContentsMargins(8, 0, 8, 0);
    tabsLayout->setSpacing(0);
    for (const QString &name : {tr("Log"), tr("Routes"), tr("Requests"), tr("Config")}) {
        auto *b = new QToolButton(tabs);
        b->setText(name);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setAutoExclusive(true);
        b->setObjectName(QStringLiteral("ktorTab"));
        tabsLayout->addWidget(b);
        const int page = int(m_tabs.size());
        connect(b, &QToolButton::clicked, this, [this, page] { showPage(page); });
        m_tabs << b;
    }
    m_tabs.first()->setChecked(true);
    tabsLayout->addStretch(1);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(logPage);
    m_stack->addWidget(m_routes);
    m_stack->addWidget(m_http);
    m_stack->addWidget(m_configView);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addSpacing(8);
    layout->addWidget(statusRow);
    auto *detailWrap = new QWidget(this);
    auto *dl = new QHBoxLayout(detailWrap);
    dl->setContentsMargins(25, 0, 10, 6);
    dl->addWidget(m_detail);
    layout->addWidget(detailWrap);
    layout->addWidget(tabs);
    layout->addWidget(m_stack, 1);

    connect(m_routes, &KtorRoutesView::locationRequested, this, &KtorPanel::locationRequested);
    // A refresh reads the sources and, when the server runs, its routing tree again.
    auto refreshAll = [this] {
        rescan();
        readLive();
    };
    connect(m_routes, &KtorRoutesView::refreshRequested, this, refreshAll);
    connect(m_routes, &KtorRoutesView::tryRequested, this, [this](const SpringEndpoint &e, bool sendNow) {
        showPage(RequestsPage);
        m_http->openEndpoint(e, sendNow);
    });
    connect(m_http, &HttpClientView::locationRequested, this, &KtorPanel::locationRequested);
    connect(m_http, &HttpClientView::openTextRequested, this, &KtorPanel::openTextRequested);
    connect(m_http, &HttpClientView::refreshRequested, this, refreshAll);
    connect(m_http, &HttpClientView::startRequested, this, [this] {
        if (m_bar)
            m_bar->start();
    });
    connect(m_http, &HttpClientView::stateChanged, this, [this] { SettingsManager::instance().setKtorRequests(m_http->state()); });
    connect(m_configView, &KtorConfigView::locationRequested, this, &KtorPanel::locationRequested);
    connect(m_configView, &KtorConfigView::addPluginsRequested, this, &KtorPanel::addPluginsRequested);
    connect(m_configView, &KtorConfigView::refreshRequested, this, refreshAll);
    connect(m_configView, &KtorConfigView::settingsRequested, this, &KtorPanel::configure);

    connect(m_level, &QComboBox::currentIndexChanged, this, [this] { m_log->setMinLevel(SpringLogView::Level(m_level->currentData().toInt())); });
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(200);
    connect(m_filterTimer, &QTimer::timeout, this, [this] { m_log->setTextFilter(m_filter->text()); });
    connect(m_filter, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_build, &QToolButton::toggled, m_log, &SpringLogView::setShowBuild);
    connect(m_pause, &QToolButton::toggled, this, [this](bool on) {
        m_log->setPaused(on);
        repaintIcons();
    });
    connect(m_clear, &QToolButton::clicked, m_log, &SpringLogView::clearLog);
    connect(m_copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_log->rawText(true)); });
    connect(m_end, &QToolButton::clicked, m_log, &SpringLogView::jumpToEnd);
    connect(m_log, &SpringLogView::countsChanged, this, &KtorPanel::updateCounts);
    connect(m_log, &SpringLogView::locationRequested, this, &KtorPanel::locationRequested);

    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(400);
    connect(m_rescanTimer, &QTimer::timeout, this, &KtorPanel::rescanIfShown);
    m_uptime->setInterval(1000);
    connect(m_uptime, &QTimer::timeout, this, &KtorPanel::refresh);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        repaintIcons();
        refresh();
    });
    repaintIcons();
    updateCounts();
    refresh();
}

void KtorPanel::repaintIcons()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral("QToolButton#ktorTab { background: transparent; border: none; border-bottom: 2px solid transparent; padding: 5px 12px; color: %1; }"
                                 "QToolButton#ktorTab:checked { background: transparent; color: %2; border-bottom: 2px solid %2; }"
                                 "QToolButton#ktorTab:hover:!checked { color: %3; }")
                      .arg(t.textMuted.name(), t.accent.name(), t.editorFg.name()));
    m_logo->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/ktor.svg"), t.accent, 16));
    m_pause->setIcon(Icons::tinted(m_pause->isChecked() ? QStringLiteral(":/new-icons/play.svg") : QStringLiteral(":/new-icons/pause.svg"), t.editorFg));
}

void KtorPanel::attach(KtorBar *bar)
{
    m_bar = bar;
    connect(m_toggle, &QToolButton::clicked, bar, &KtorBar::toggle);
    connect(m_restart, &QToolButton::clicked, bar, &KtorBar::restart);
    connect(m_config, &QToolButton::clicked, this, &KtorPanel::configure);
    connect(bar->server(), &DevServer::output, m_log, &SpringLogView::feed);
    connect(bar->server(), &DevServer::stateChanged, this, &KtorPanel::onStateChanged);
    connect(bar, &KtorBar::projectChanged, this, &KtorPanel::sourcesChanged);
    // Auto-reload, step by step in the log.
    connect(bar->reloader(), &KtorReloader::output, m_log, &SpringLogView::feed);
    connect(bar->reloader(), &KtorReloader::stateChanged, this, [this] {
        const KtorReloader *r = m_bar->reloader();
        using St = KtorReloader::State;
        const St st = r->state();
        const St was = St(m_lastReloaderState);
        m_lastReloaderState = int(st);
        if (st == St::Compiling && was == St::Off)
            m_log->addMarker(tr("Auto-reload: watching the sources · %1").arg(now()));
        else if (st == St::Compiling)
            m_log->addMarker(tr("Compiling the changes · %1").arg(now()));
        else if (st == St::Failed) {
            const QStringList errors = r->errors();
            QString first = errors.value(0);
            if (errors.size() > 1)
                first += tr(" (+%n more)", nullptr, int(errors.size() - 1));
            m_log->addMarker(tr("Compile failed · %1 · %2").arg(now(), first));
        } else if (st == St::Off && !r->failure().isEmpty())
            m_log->addMarker(tr("Auto-reload stopped · %1 · %2").arg(now(), r->failure()));
        refresh();
        updateConfig();
    });
    connect(bar, &KtorBar::reloadMarker, this, [this](const QString &text) { m_log->addMarker(QStringLiteral("%1 · %2").arg(text, now())); });
    connect(bar->server(), &KtorServer::reloaded, this, [this](double seconds) {
        m_log->addMarker(tr("Reloaded in %1 s · %2").arg(seconds, 0, 'f', 2).arg(now()));
        m_stale = true; // the classes changed: read the sources again too
        rescanIfShown();
        readLive();
    });
    connect(bar->server(), &KtorServer::reloadFailed, this, [this] { m_log->addMarker(tr("Reload failed · %1 · the server keeps running without the application until the next change").arg(now())); });
    connect(bar, &KtorBar::rootChanged, this, [this] {
        m_log->clearLog();
        m_lastState = -1;
        m_code = KtorScan();
        m_scan = KtorScan();
        dropLive();
        m_stale = true;
        // The project's settings are loaded around the same time: read the saved requests once that is done.
        QTimer::singleShot(0, this, [this] {
            loadRequests();
            rescanIfShown();
        });
        refresh();
    });
    loadRequests();
    refresh();
}

void KtorPanel::configure()
{
    if (!m_bar)
        return;
    m_bar->configure();
    updateConfig(); // the port / development mode shown may have changed
    refresh();
}

void KtorPanel::loadRequests()
{
    m_http->clearForNewProject();
    m_http->setState(SettingsManager::instance().ktorRequests());
    m_http->setScan(m_scan.http);
}

void KtorPanel::showPage(int page)
{
    m_stack->setCurrentIndex(page);
    m_tabs.at(page)->setChecked(true);
    if (page != LogPage && m_stale)
        rescan();
    else if (page == ConfigPage)
        updateConfig(); // the configuration file may have been edited
}

void KtorPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, &KtorPanel::rescanIfShown); // edits made while the panel was hidden
}

void KtorPanel::sourcesChanged()
{
    m_stale = true;
    m_rescanTimer->start();
}

void KtorPanel::rescanIfShown()
{
    if (m_stale && isVisible() && m_stack->currentIndex() != LogPage)
        rescan();
}

void KtorPanel::setResolver(const KtorLive::Resolver &resolver)
{
    m_resolver = resolver;
    m_log->setResolver(resolver);
}

void KtorPanel::rescan()
{
    m_stale = false;
    m_code = m_bar && m_bar->isAvailable() ? KtorSource::scan(m_bar->project().sourceRoots()) : KtorScan();
    applyScan();
}

void KtorPanel::applyScan()
{
    if (m_bar) {
        m_bar->setSourceFacts(m_code);
        if (m_live.ok)
            m_bar->setCodeReloadable(m_live.codeReloadable());
    }
    m_scan = m_live.ok ? KtorLive::merge(m_code, m_live, m_resolver) : m_code;
    const KtorServer *server = m_bar ? m_bar->server() : nullptr;
    const bool running = server && server->state() == DevServer::State::Running;
    if (m_live.ok) {
        m_routes->setOrigin(tr("read from the running server"),
                            tr("The routes are the running server's own routing tree, read through a local debug connection: "
                               "what it really serves, however the code builds it. Parameters, bodies and lines come from the sources."));
    } else if (running && !m_live.error.isEmpty()) {
        m_routes->setOrigin(tr("from the sources"), tr("The running server's routes could not be read: %1").arg(m_live.error));
    } else if (running && m_bar && !m_bar->options().liveRoutes) {
        m_routes->setOrigin(tr("from the sources"), tr("Reading the routes of the running server is off in the Ktor settings."));
    } else if (running) {
        m_routes->setOrigin(tr("from the sources"), tr("The server runs without a debug port (custom command): the routes are what the sources declare."));
    } else {
        m_routes->setOrigin(tr("from the sources"), tr("Read from the Kotlin sources. Start the server to see the routes it really serves."));
    }
    m_routes->setScan(m_scan);
    m_http->setScan(m_scan.http);
    updateConfig();
    refresh();
}

void KtorPanel::readLive()
{
    const KtorServer *server = m_bar ? m_bar->server() : nullptr;
    if (!server || server->state() != DevServer::State::Running || server->debugPort() <= 0)
        return;
    if (m_liveBusy) {
        m_liveAgain = true; // the debug port takes one connection at a time
        return;
    }
    m_liveBusy = true;
    const int port = server->debugPort();
    const int generation = m_liveGeneration;
    auto result = std::make_shared<KtorLiveResult>();
    QThread *thread = QThread::create([result, port] { *result = KtorLive::read(port); });
    connect(thread, &QThread::finished, this, [this, thread, result, generation] {
        thread->deleteLater();
        m_liveBusy = false;
        if (generation == m_liveGeneration) {
            m_live = *result;
            if (m_code.files == 0 && m_code.routes.isEmpty())
                m_code = KtorSource::scan(m_bar->project().sourceRoots()); // for parameters, bodies and lines
            applyScan();
        }
        if (m_liveAgain) {
            m_liveAgain = false;
            readLive();
        }
    });
    thread->start();
}

void KtorPanel::dropLive()
{
    ++m_liveGeneration; // a read under way belongs to a server that is gone
    m_liveAgain = false;
    if (!m_live.ok && m_live.error.isEmpty())
        return;
    m_live = KtorLiveResult();
    applyScan();
}

KtorConfigView::Port KtorPanel::configuredPort() const
{
    KtorConfigView::Port p;
    if (!m_bar || !m_bar->isAvailable())
        return p;
    if (m_bar->options().port > 0) {
        p.port = m_bar->options().port;
        p.source = tr("run settings");
        return p;
    }
    const QString config = m_bar->project().configFile();
    for (const KtorProject::ConfigEntry &e : KtorProject::readConfig(config)) {
        if (e.key != QLatin1String("ktor.deployment.port"))
            continue;
        p.raw = e.value;
        if (const int n = KtorProject::portOf(e.value); n > 0) {
            p.port = n;
            p.source = QFileInfo(config).fileName();
            return p;
        }
    }
    if (m_scan.embeddedPort > 0) {
        p.port = m_scan.embeddedPort;
        p.source = QStringLiteral("embeddedServer");
        return p;
    }
    p.source = tr("default");
    return p;
}

QString KtorPanel::baseUrl() const
{
    const KtorServer *server = m_bar ? m_bar->server() : nullptr;
    const int port = server && server->state() == DevServer::State::Running && server->port() > 0 ? server->port() : configuredPort().port;
    return QStringLiteral("http://localhost:%1").arg(port);
}

// Not part of refresh(): that runs every second for the uptime, the configuration only changes with these events.
void KtorPanel::updateConfig()
{
    if (!m_bar)
        return;
    const KtorServer *server = m_bar->server();
    const bool running = server && server->state() == DevServer::State::Running;
    const KtorProject::Options o = m_bar->options();
    QString reload;
    if (!o.command.trimmed().isEmpty())
        reload = tr("not available with a custom command");
    else if (!o.autoReload)
        reload = tr("off — turn it on with the bar's ⚡ switch or in the run settings");
    else if (const QString why = m_bar->restartReason(); !why.isEmpty())
        reload = tr("restarts the server after each compile: %1").arg(why);
    else
        reload = m_bar->project().maven ? tr("in place — mvn compile after each save, Ktor reloads the classes")
                                        : tr("in place — a continuous build compiles every change, Ktor reloads the classes");
    if (m_bar->reloadSession() && m_bar->reloader()->state() == KtorReloader::State::Failed)
        reload += tr(" · last compile failed");
    QString routes;
    if (m_live.ok)
        routes = tr("%n from the running server", nullptr, int(std::count_if(m_scan.routes.cbegin(), m_scan.routes.cend(), [](const KtorRoute &r) {
                        return !r.notServed;
                    })));
    else if (running && !m_live.error.isEmpty())
        routes = tr("from the sources (the server could not be read: %1)").arg(m_live.error);
    else
        routes = o.liveRoutes && o.command.trimmed().isEmpty() ? tr("from the sources; read from the server while it runs") : tr("from the sources");
    m_configView->setData(m_bar->project(), o, m_scan, configuredPort(), running, running ? server->port() : 0, reload, routes);
}

void KtorPanel::onStateChanged()
{
    if (!m_bar || !m_bar->server())
        return;
    const int state = int(m_bar->server()->state());
    if (state != m_lastState) {
        using S = DevServer::State;
        switch (S(state)) {
        case S::Starting:
            m_stale = true; // a start follows edits: the routes may differ
            dropLive();
            m_log->addMarker(tr("Starting · %1").arg(now()));
            m_log->jumpToEnd();
            break;
        case S::Stopped:
            dropLive();
            if (m_lastState == int(S::Starting) || m_lastState == int(S::Running))
                m_log->addMarker(tr("Stopped · %1").arg(now()));
            break;
        case S::Failed:
            dropLive();
            m_log->addMarker(tr("Failed · %1 · %2").arg(now(), m_bar->server()->errorString()));
            break;
        case S::Running:
            rescanIfShown();
            readLive();
            break;
        }
        m_lastState = state;
        updateConfig();
    }
    refresh();
}

void KtorPanel::updateCounts()
{
    const int total = m_log->totalCount(), shown = m_log->shownCount();
    m_count->setText(shown == total ? tr("%n entries", nullptr, total) : tr("%1 of %n entries", nullptr, total).arg(shown));
    m_log->setPlaceholderText(total > 0 && shown == 0 ? tr("No entries match the filters.") : tr("Start the server to see its output here."));
}

void KtorPanel::refresh()
{
    KtorServer *server = m_bar ? m_bar->server() : nullptr;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QColor colour = t.idle;
    QString status = tr("Stopped"), detail;
    bool active = false;
    const bool running = server && server->state() == DevServer::State::Running;
    if (server) {
        using S = DevServer::State;
        active = server->isActive();
        switch (server->state()) {
        case S::Stopped:
            break;
        case S::Starting:
            colour = t.warning;
            status = server->phaseText();
            break;
        case S::Running:
            colour = t.success;
            status = server->port() > 0 ? tr("Responding at <a href=\"%1\" style=\"color:%2; text-decoration:none\">localhost:%3</a>")
                                              .arg(server->url(), t.success.name()).arg(server->port())
                                        : tr("Running");
            break;
        case S::Failed:
            colour = t.danger;
            status = tr("Failed — %1").arg(server->errorString().toHtmlEscaped());
            break;
        }
        QStringList bits;
        if (m_bar->isAvailable()) {
            const KtorProject &p = m_bar->project();
            bits << p.appName;
            if (!p.engine.isEmpty())
                bits << p.engine;
            bits << (p.version.isEmpty() ? tr("Ktor") : tr("Ktor %1").arg(p.version));
            if (m_bar->reloadSession())
                bits << (m_bar->restartReason().isEmpty() ? tr("auto-reload") : tr("auto-reload (restarts)"));
            else if (m_bar->options().development)
                bits << tr("development");
        }
        if (running) {
            if (server->startupSeconds() > 0)
                bits << tr("started in %1 s").arg(server->startupSeconds(), 0, 'f', 2);
            if (server->uptimeMs() > 0)
                bits << tr("up %1").arg(duration(server->uptimeMs()));
            if (server->lastReloadSeconds() > 0)
                bits << tr("reloaded in %1 s").arg(server->lastReloadSeconds(), 0, 'f', 2);
            // Auto-reload's current step after the address.
            const KtorReloader *r = m_bar->reloader();
            QString step;
            if (m_bar->reloadSession() && r->state() == KtorReloader::State::Compiling)
                step = tr("compiling…");
            else if (server->isReloading())
                step = tr("reloading…");
            else if (m_bar->reloadSession() && r->state() == KtorReloader::State::Failed)
                step = QStringLiteral("<span style=\"color:%1\">%2</span>").arg(t.danger.name(), tr("compile error"));
            if (!step.isEmpty()) {
                status += QStringLiteral(" · ") + step;
                if (!step.startsWith(QLatin1Char('<')))
                    colour = t.warning;
            }
        }
        detail = bits.join(QStringLiteral(" · "));
        if (m_bar->isAvailable())
            m_detail->setToolTip(m_bar->command());
        if (running && !m_uptime->isActive())
            m_uptime->start();
        else if (!running)
            m_uptime->stop();
    }
    if (m_bar) {
        const QString base = baseUrl();
        m_http->setBaseUrl(base);
        m_http->setServerRunning(running);
        m_routes->setServer(base, running);
    }
    m_dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(colour.name()));
    m_status->setTextFormat(Qt::RichText);
    m_status->setText(status);
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
    m_toggle->setIcon(Icons::tinted(active ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/play.svg"), active ? t.danger : t.success));
    m_toggle->setToolTip(active ? tr("Stop the server") : tr("Start the server"));
    m_toggle->setEnabled(m_bar && m_bar->isAvailable());
    m_restart->setEnabled(server && server->state() != DevServer::State::Stopped);
    m_config->setEnabled(m_bar && m_bar->isAvailable());
}
