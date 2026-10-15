#include "spring/SpringPanel.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "spring/ActuatorView.h"
#include "spring/HttpClientView.h"
#include "spring/SpringBar.h"
#include "spring/SpringEndpoints.h"
#include "spring/SpringLogView.h"
#include "spring/SpringServer.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

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

SpringPanel::SpringPanel(QWidget *parent) : QWidget(parent), m_uptime(new QTimer(this)), m_filterTimer(new QTimer(this))
{
    auto button = [this](const QString &tip, bool checkable = false) {
        auto *b = new QToolButton(this);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        return b;
    };

    m_logo = new QLabel(this);
    m_logo->setFixedSize(18, 18);
    m_logo->setScaledContents(true);
    m_title = new QLabel(tr("SPRING BOOT"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_toggle = button(QString());
    m_restart = button(tr("Restart the application"));
    m_config = button(tr("Profiles, port and arguments"));
    m_closeBtn = button(tr("Close Spring Boot panel"));
    Icons::bind(m_restart, QStringLiteral(":/new-icons/refresh-cw.svg"));
    Icons::bind(m_config, QStringLiteral(":/new-icons/cog.svg"));
    Icons::bind(m_closeBtn, QStringLiteral(":/new-icons/x.svg"));
    connect(m_closeBtn, &QToolButton::clicked, this, &SpringPanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 4, 0);
    hl->setSpacing(2);
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
    m_log->setPlaceholderText(tr("Start the application to see its output here."));
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("emptyText"));
    m_count->setContentsMargins(10, 3, 10, 4);
    m_count->setStyleSheet(QStringLiteral("QLabel { border-top: 1px solid palette(shadow); }"));

    m_http = new HttpClientView(this);
    m_actuator = new ActuatorView(this);

    // Log | Requests
    auto *tabs = new QWidget(this);
    auto *tabsLayout = new QHBoxLayout(tabs);
    tabsLayout->setContentsMargins(8, 0, 8, 0);
    tabsLayout->setSpacing(0);
    m_tabLog = new QToolButton(tabs);
    m_tabRequests = new QToolButton(tabs);
    m_tabLog->setText(tr("Log"));
    m_tabRequests->setText(tr("Requests"));
    m_tabActuator = new QToolButton(tabs);
    m_tabActuator->setText(tr("Actuator"));
    m_tabActuator->setVisible(false);
    for (QToolButton *b : {m_tabLog, m_tabRequests, m_tabActuator}) {
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setAutoExclusive(true);
        b->setObjectName(QStringLiteral("springTab"));
        tabsLayout->addWidget(b);
    }
    m_tabLog->setChecked(true);
    tabsLayout->addStretch(1);

    auto *logPage = new QWidget(this);
    auto *lp = new QVBoxLayout(logPage);
    lp->setContentsMargins(0, 0, 0, 0);
    lp->setSpacing(0);
    lp->addWidget(tools);
    lp->addWidget(m_log, 1);
    lp->addWidget(m_count);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(logPage);
    m_stack->addWidget(m_http);
    m_stack->addWidget(m_actuator);

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

    connect(m_tabLog, &QToolButton::clicked, this, [this] { showPage(0); });
    connect(m_tabRequests, &QToolButton::clicked, this, [this] { showPage(1); });
    connect(m_tabActuator, &QToolButton::clicked, this, [this] { showPage(2); });
    connect(m_actuator, &ActuatorView::openFileRequested, this, &SpringPanel::openFileRequested);
    connect(m_actuator, &ActuatorView::startRequested, this, [this] {
        if (m_bar)
            m_bar->start();
    });
    connect(m_http, &HttpClientView::locationRequested, this, &SpringPanel::locationRequested);
    connect(m_http, &HttpClientView::refreshRequested, this, &SpringPanel::scanEndpoints);
    connect(m_http, &HttpClientView::startRequested, this, [this] {
        if (m_bar)
            m_bar->start();
    });
    connect(m_http, &HttpClientView::stateChanged, this, [this] { SettingsManager::instance().setSpringRequests(m_http->state()); });

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
    connect(m_log, &SpringLogView::countsChanged, this, &SpringPanel::updateCounts);
    connect(m_log, &SpringLogView::locationRequested, this, &SpringPanel::locationRequested);

    m_uptime->setInterval(1000);
    connect(m_uptime, &QTimer::timeout, this, &SpringPanel::refresh);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        repaintIcons();
        refresh();
    });
    repaintIcons();
    updateCounts();
    refresh();
}

void SpringPanel::repaintIcons()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral("QToolButton#springTab { background: transparent; border: none; border-bottom: 2px solid transparent; padding: 5px 12px; color: %1; }"
                                 "QToolButton#springTab:checked { background: transparent; color: %2; border-bottom: 2px solid %2; }"
                                 "QToolButton#springTab:hover:!checked { color: %3; }")
                      .arg(t.textMuted.name(), t.accent.name(), t.editorFg.name()));
    m_logo->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/spring.svg"), t.success, 18));
    // Checked = paused: the button then offers to resume.
    m_pause->setIcon(Icons::tinted(m_pause->isChecked() ? QStringLiteral(":/new-icons/play.svg") : QStringLiteral(":/new-icons/pause.svg"), t.editorFg));
}

void SpringPanel::attach(SpringBar *bar)
{
    m_bar = bar;
    connect(m_toggle, &QToolButton::clicked, bar, &SpringBar::toggle);
    connect(m_restart, &QToolButton::clicked, bar, &SpringBar::restart);
    connect(m_config, &QToolButton::clicked, bar, &SpringBar::configure);
    connect(bar->server(), &DevServer::output, m_log, &SpringLogView::feed);
    connect(bar->server(), &DevServer::stateChanged, this, &SpringPanel::onStateChanged);
    connect(bar, &SpringBar::rootChanged, this, [this] {
        m_log->clearLog();
        m_lastState = -1;
        // The project's settings are loaded around the same time: read the saved requests once that is done.
        QTimer::singleShot(0, this, [this] { loadRequests(); updateTabs(); });
        refresh();
    });
    loadRequests();
    updateTabs();
    refresh();
}

void SpringPanel::loadRequests()
{
    m_http->clearForNewProject();
    m_http->setState(SettingsManager::instance().springRequests());
    m_endpointsStale = true;
    if (m_stack->currentIndex() == 1)
        scanEndpoints();
}

void SpringPanel::showPage(int page)
{
    m_stack->setCurrentIndex(page);
    (page == 0 ? m_tabLog : page == 1 ? m_tabRequests : m_tabActuator)->setChecked(true);
    if (page == 2)
        m_actuator->refresh();
    if (page == 1)
        scanEndpoints(); // controllers may have changed since the last visit
}

void SpringPanel::updateTabs()
{
    const bool actuator = m_bar && m_bar->isAvailable() && m_bar->project().hasActuator();
    m_tabActuator->setVisible(actuator);
    if (!actuator && m_stack->currentIndex() == 2)
        showPage(0);
}

void SpringPanel::scanEndpoints()
{
    m_endpointsStale = false;
    if (!m_bar || !m_bar->isAvailable()) {
        m_http->setScan(SpringScan());
        return;
    }
    m_http->setScan(SpringEndpoints::scan(m_bar->project().sourceRoots()));
}

void SpringPanel::onStateChanged()
{
    if (!m_bar || !m_bar->server())
        return;
    const int state = int(m_bar->server()->state());
    if (state != m_lastState) {
        const QString now = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        using S = DevServer::State;
        switch (S(state)) {
        case S::Starting:
            m_endpointsStale = true;
            m_log->addMarker(tr("Starting · %1").arg(now));
            m_log->jumpToEnd(); // a new run: show it even when the old one was scrolled
            break;
        case S::Stopped:
            if (m_lastState == int(S::Starting) || m_lastState == int(S::Running))
                m_log->addMarker(tr("Stopped · %1").arg(now));
            break;
        case S::Failed:
            m_log->addMarker(tr("Failed · %1 · %2").arg(now, m_bar->server()->errorString()));
            break;
        case S::Running:
            if (m_stack->currentIndex() == 1)
                scanEndpoints();
            break;
        }
        m_lastState = state;
        updateTabs();
    }
    refresh();
}

void SpringPanel::updateCounts()
{
    const int total = m_log->totalCount(), shown = m_log->shownCount();
    m_count->setText(shown == total ? tr("%n entries", nullptr, total) : tr("%1 of %n entries", nullptr, total).arg(shown));
    if (total > 0 && shown == 0)
        m_log->setPlaceholderText(tr("No entries match the filters."));
    else
        m_log->setPlaceholderText(tr("Start the application to see its output here."));
}

void SpringPanel::refresh()
{
    SpringServer *server = m_bar ? m_bar->server() : nullptr;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QColor colour = t.idle;
    QString status = tr("Stopped"), detail;
    bool active = false;
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
            if (server->port() > 0)
                status = tr("Running on <a href=\"%1\" style=\"color:%2; text-decoration:none\">localhost:%3</a>")
                             .arg(server->url(), t.success.name()).arg(server->port());
            else
                status = tr("Running");
            break;
        case S::Failed:
            colour = t.danger;
            status = tr("Failed — %1").arg(server->errorString().toHtmlEscaped());
            break;
        }
        QStringList bits;
        if (m_bar->isAvailable())
            bits << m_bar->project().appName << (m_bar->project().maven ? tr("Maven") : tr("Gradle"));
        const SpringProject::Options o = m_bar->options();
        if (!o.profiles.isEmpty())
            bits << tr("profile %1").arg(o.profiles);
        if (server->state() == S::Running) {
            if (server->startupSeconds() > 0)
                bits << tr("started in %1 s").arg(server->startupSeconds(), 0, 'f', 1);
            if (server->uptimeMs() > 0)
                bits << tr("up %1").arg(duration(server->uptimeMs()));
        }
        detail = bits.join(QStringLiteral(" · "));
        if (m_bar->isAvailable())
            m_detail->setToolTip(m_bar->command());
        if (server->state() == S::Running && !m_uptime->isActive())
            m_uptime->start();
        else if (server->state() != S::Running)
            m_uptime->stop();
    }
    if (m_bar) {
        const int port = server && server->port() > 0 ? server->port() : m_bar->options().port > 0 ? m_bar->options().port : 8080;
        m_http->setBaseUrl(QStringLiteral("http://localhost:%1").arg(port));
        m_http->setServerRunning(server && server->state() == DevServer::State::Running);
        m_actuator->setTarget(QStringLiteral("http://localhost:%1").arg(port), m_bar->project().resourcesDir(), server && server->state() == DevServer::State::Running);
    }
    m_dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(colour.name()));
    m_status->setTextFormat(Qt::RichText);
    m_status->setText(status);
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
    m_toggle->setIcon(Icons::tinted(active ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/play.svg"),
                                    active ? t.danger : t.success));
    m_toggle->setToolTip(active ? tr("Stop the application") : tr("Start the application"));
    m_toggle->setEnabled(m_bar && m_bar->isAvailable());
    m_restart->setEnabled(server && server->state() != DevServer::State::Stopped);
    m_config->setEnabled(m_bar && m_bar->isAvailable());
}
