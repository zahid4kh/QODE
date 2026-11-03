#include "ktor/KtorBar.h"

#include "ktor/KtorConfigDialog.h"
#include "ktor/KtorReloader.h"
#include "ktor/KtorServer.h"
#include "ktor/KtorSource.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "spring/SpringProject.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

KtorBar::KtorBar(QWidget *parent)
    : QWidget(parent), m_server(new KtorServer(this)), m_reloader(new KtorReloader(this)), m_net(new QNetworkAccessManager(this)),
      m_reloadWait(new QTimer(this)), m_watcher(new QFileSystemWatcher(this)), m_redetect(new QTimer(this))
{
    m_net->setProxy(QNetworkProxy::NoProxy);
    m_redetect->setSingleShot(true);
    m_redetect->setInterval(500);
    connect(m_redetect, &QTimer::timeout, this, &KtorBar::redetect);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_redetect, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::fileChanged, m_redetect, qOverload<>(&QTimer::start));

    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(4, 0, 4, 0);
    l->setSpacing(2);
    m_dot = new QLabel(this);
    m_dot->setFixedSize(8, 8);
    m_text = new QLabel(this);
    m_text->setContentsMargins(4, 0, 6, 0);
    l->addWidget(m_dot, 0, Qt::AlignVCenter);
    l->addWidget(m_text, 0, Qt::AlignVCenter);

    auto button = [this, l](const QString &tip) {
        auto *b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        b->setToolTip(tip);
        l->addWidget(b);
        return b;
    };
    m_toggle = button(QString());
    m_restart = button(tr("Restart the Ktor server"));
    m_reload = button(QString());
    m_reload->setCheckable(true);
    m_panel = button(tr("Show the Ktor panel (log, routes, requests, configuration)"));
    m_config = button(tr("Ktor settings (port, development mode, arguments)"));
    Icons::bind(m_restart, QStringLiteral(":/new-icons/refresh-cw.svg"));
    Icons::bind(m_reload, QStringLiteral(":/new-icons/zap.svg"));
    Icons::bind(m_panel, QStringLiteral(":/new-icons/ktor.svg"));
    Icons::bind(m_config, QStringLiteral(":/new-icons/cog.svg"));

    connect(m_toggle, &QToolButton::clicked, this, &KtorBar::toggle);
    connect(m_restart, &QToolButton::clicked, this, &KtorBar::restart);
    connect(m_reload, &QToolButton::clicked, this, &KtorBar::setAutoReload);
    connect(m_panel, &QToolButton::clicked, this, &KtorBar::panelRequested);
    connect(m_config, &QToolButton::clicked, this, &KtorBar::configure);
    connect(m_text, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    connect(m_server, &DevServer::stateChanged, this, [this] {
        onServerState();
        refresh();
        emit stateChanged();
    });
    connect(m_reloader, &KtorReloader::stateChanged, this, [this] {
        refresh();
        emit stateChanged();
    });
    connect(m_reloader, &KtorReloader::compiled, this, &KtorBar::onCompiled);
    // Ktor's own lines say the reload happened (or failed): no restart needed.
    auto reported = [this] {
        m_awaitingReload = false;
        m_reloadWait->stop();
    };
    connect(m_server, &KtorServer::reloadStarted, this, reported);
    connect(m_server, &KtorServer::reloaded, this, reported);
    connect(m_server, &KtorServer::reloadFailed, this, reported);
    m_reloadWait->setSingleShot(true);
    m_reloadWait->setInterval(3000);
    connect(m_reloadWait, &QTimer::timeout, this, [this] {
        if (!m_awaitingReload || !m_session)
            return;
        m_awaitingReload = false;
        emit reloadMarker(tr("Ktor did not report a reload — restarting the server"));
        launch();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refresh(); });
    refresh();
}

bool KtorBar::isActive() const
{
    return m_server && m_server->isActive();
}

KtorProject::Options KtorBar::options() const
{
    const QJsonObject cfg = SettingsManager::instance().ktorServer();
    KtorProject::Options o;
    o.port = cfg.value(QStringLiteral("port")).toInt();
    o.development = cfg.value(QStringLiteral("development")).toBool();
    o.autoReload = cfg.value(QStringLiteral("autoReload")).toBool();
    o.liveRoutes = cfg.value(QStringLiteral("liveRoutes")).toBool(true);
    o.arguments = cfg.value(QStringLiteral("arguments")).toString();
    o.command = cfg.value(QStringLiteral("command")).toString();
    o.jvmArgs = cfg.value(QStringLiteral("jvmArgs")).toString();
    o.env = cfg.value(QStringLiteral("env")).toString();
    return o;
}

QString KtorBar::command() const
{
    return m_project.command(options());
}

void KtorBar::setProjectRoot(const QString &root)
{
    endSession();
    if (m_server)
        m_server->stop();
    const bool changed = root != m_root;
    m_root = root;
    redetect();
    if (changed)
        emit rootChanged();
}

void KtorBar::redetect()
{
    const bool was = isAvailable();
    m_project = KtorProject::detect(m_root);
    // The root folder catches a build file appearing; the files catch edits (editors may replace them).
    const QStringList watched = m_watcher->files() + m_watcher->directories();
    if (!watched.isEmpty())
        m_watcher->removePaths(watched);
    if (!m_root.isEmpty()) {
        m_watcher->addPath(m_root);
        QStringList dirs{m_root};
        if (m_project.valid && m_project.moduleDir != m_root)
            dirs << m_project.moduleDir;
        for (const QString &d : std::as_const(dirs))
            for (const char *f : {"build.gradle.kts", "build.gradle", "pom.xml", "gradle/libs.versions.toml", "settings.gradle.kts"})
                if (QFileInfo::exists(d + QLatin1Char('/') + QLatin1String(f)))
                    m_watcher->addPath(d + QLatin1Char('/') + QLatin1String(f));
    }
    refresh();
    if (was != isAvailable())
        emit availabilityChanged(isAvailable());
    emit projectChanged();
}

void KtorBar::start()
{
    if (!m_server)
        return;
    if (!m_server->isActive())
        redetect(); // pick up a changed build file
    if (!m_server || !isAvailable())
        return;
    emit starting();
    const KtorProject::Options o = options();
    m_session = o.autoReload && o.command.trimmed().isEmpty();
    if (m_session)
        setSourceFacts(KtorSource::scan(m_project.sourceRoots()));
    else
        m_reloader->stop();
    launch();
}

void KtorBar::launch()
{
    const KtorProject::Options o = options();
    m_awaitingReload = false;
    m_reloadWait->stop();
    m_codeReloadable = -1; // the new process answers again
    int debugPort = 0;
    if (o.liveRoutes && o.command.trimmed().isEmpty()) {
        // A port of our choosing: with address=…:0 the JVM would listen on a new one after every detach.
        QTcpServer probe;
        if (probe.listen(QHostAddress::LocalHost, 0))
            debugPort = probe.serverPort();
    }
    if (!m_project.maven && o.command.trimmed().isEmpty()) {
        const QString path = m_project.runScriptPath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(m_project.runScript(o, debugPort).toUtf8());
    }
    m_server->setExtraEnvironment(SpringProject::parseEnv(o.env));
    m_server->start(m_project.command(o, debugPort), m_project.workDir, 0);
}

void KtorBar::endSession()
{
    m_session = false;
    m_awaitingReload = false;
    m_reloadWait->stop();
    if (m_reloader)
        m_reloader->stop();
}

QString KtorBar::restartReason() const
{
    if (!m_server)
        return {};
    if (!m_server->reloadBlocker().isEmpty())
        return m_server->reloadBlocker();
    if (m_codeReloadable == 0 || (m_codeReloadable < 0 && !m_moduleReloadable))
        return tr("Ktor cannot load this module again, so a reload would keep the old code (embeddedServer gets a lambda or "
                  "a plain fun; declaring it suspend fun Application.module() or listing it in application.yaml lets Ktor reload in place)");
    return {};
}

void KtorBar::setCodeReloadable(int reloadable)
{
    if (m_codeReloadable == reloadable)
        return;
    m_codeReloadable = reloadable;
    refresh();
    emit stateChanged();
}

void KtorBar::setSourceFacts(const KtorScan &scan)
{
    const bool reloadable = scan.embeddedEngine.isEmpty() || scan.embeddedModuleReloadable;
    if (m_moduleReloadable == reloadable)
        return;
    m_moduleReloadable = reloadable;
    refresh();
    emit stateChanged();
}

// The continuous build starts once the start's own build is done (two builds compiling the same classes at once get in
// each other's way): when the server is launched ("> Task :run"), runs, or failed to start.
void KtorBar::onServerState()
{
    if (!m_session || !m_reloader || m_reloader->isOn())
        return;
    using S = DevServer::State;
    const S st = m_server->state();
    if ((st == S::Starting && m_server->phase() == KtorServer::Phase::Starting) || st == S::Running || st == S::Failed)
        m_reloader->start(m_project, SpringProject::parseEnv(options().env));
}

void KtorBar::onCompiled()
{
    if (!m_session || !m_server)
        return;
    using S = DevServer::State;
    const QString reason = restartReason();
    if (m_server->state() == S::Running && reason.isEmpty()) {
        requestReload();
        return;
    }
    if (m_server->state() == S::Running)
        emit reloadMarker(tr("Restarting the server: %1").arg(reason));
    else if (m_server->state() == S::Starting)
        emit reloadMarker(tr("Restarting: the sources changed while the server was starting"));
    else
        emit reloadMarker(tr("Starting the server again with the new classes"));
    launch();
}

// Ktor looks for changed classes when a call comes in (EmbeddedServer.currentApplication): one request that no route
// answers makes it reload now rather than on the next real request, so errors and the new routes show up right away.
void KtorBar::requestReload()
{
    m_awaitingReload = true;
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/__qode_reload__").arg(m_server->port())));
    req.setTransferTimeout(30000);
    QNetworkReply *reply = m_net->head(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        // Gradle forwards the server's output: its log lines can come after the response.
        if (m_awaitingReload)
            m_reloadWait->start();
    });
}

void KtorBar::stop()
{
    endSession();
    if (m_server)
        m_server->stop();
}

void KtorBar::setAutoReload(bool on)
{
    QJsonObject cfg = SettingsManager::instance().ktorServer();
    if (on)
        cfg.insert(QStringLiteral("autoReload"), true);
    else
        cfg.remove(QStringLiteral("autoReload"));
    SettingsManager::instance().setKtorServer(cfg);
    refresh();
    emit stateChanged();
    if (isActive())
        restart(); // the server starts again in (or out of) development mode with its watch paths
}

void KtorBar::sourceSaved(const QString &path)
{
    if (m_session && m_reloader && m_project.maven && path.startsWith(m_project.moduleDir + QLatin1Char('/')))
        m_reloader->compileSoon();
}

void KtorBar::restart()
{
    if (m_project.valid)
        start(); // start() on a live server stops it first and starts again with the current settings
}

void KtorBar::toggle()
{
    if (isActive())
        stop();
    else
        start();
}

void KtorBar::shutdown()
{
    m_session = false;
    delete m_reloader; // the destructors end the process groups
    m_reloader = nullptr;
    delete m_server;
    m_server = nullptr;
}

void KtorBar::configure()
{
    if (!m_project.valid)
        return;
    KtorConfigDialog dlg(m_project, options(), window());
    connect(&dlg, &KtorConfigDialog::openFileRequested, this, [this, &dlg](const QString &p) {
        emit openFileRequested(p);
        dlg.reject();
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    const KtorProject::Options o = dlg.options();
    QJsonObject cfg;
    if (o.development)
        cfg.insert(QStringLiteral("development"), true);
    if (o.autoReload)
        cfg.insert(QStringLiteral("autoReload"), true);
    if (!o.liveRoutes)
        cfg.insert(QStringLiteral("liveRoutes"), false);
    if (o.port > 0)
        cfg.insert(QStringLiteral("port"), o.port);
    if (!o.arguments.isEmpty())
        cfg.insert(QStringLiteral("arguments"), o.arguments);
    if (!o.command.isEmpty())
        cfg.insert(QStringLiteral("command"), o.command);
    if (!o.jvmArgs.isEmpty())
        cfg.insert(QStringLiteral("jvmArgs"), o.jvmArgs);
    if (!o.env.isEmpty())
        cfg.insert(QStringLiteral("env"), o.env);
    SettingsManager::instance().setKtorServer(cfg);
    refresh();
    if (isActive())
        restart(); // apply the new port / mode right away
}

void KtorBar::refresh()
{
    if (!m_server)
        return;
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    const QString green = theme.success.name();
    const QString red = theme.danger.name();
    const QString amber = theme.warning.name();
    const QString grey = theme.idle.name();

    QString colour = grey, text;
    bool rich = false;
    switch (m_server->state()) {
    case DevServer::State::Stopped:
        text = tr("Ktor stopped");
        break;
    case DevServer::State::Starting:
        colour = amber;
        text = m_server->phaseText();
        break;
    case DevServer::State::Running:
        colour = green;
        if (m_server->port() > 0) {
            rich = true;
            text = QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">localhost:%3</a>")
                       .arg(m_server->url(), green).arg(m_server->port());
        } else {
            text = tr("Running");
        }
        break;
    case DevServer::State::Failed:
        colour = red;
        text = m_server->errorString();
        break;
    }
    // Auto-reload: what the loop is doing, after the address.
    QString reloadNote;
    QString noteColour = theme.textMuted.name();
    if (m_session && m_reloader) {
        if (m_reloader->state() == KtorReloader::State::Compiling) {
            reloadNote = tr("compiling…");
            colour = amber;
        } else if (m_server->isReloading()) {
            reloadNote = tr("reloading…");
            colour = amber;
        } else if (m_reloader->state() == KtorReloader::State::Failed) {
            reloadNote = tr("compile error");
            noteColour = red;
        }
    }
    if (!reloadNote.isEmpty() && m_server->state() == DevServer::State::Running) {
        if (!rich) {
            text = text.toHtmlEscaped();
            rich = true;
        }
        text += QStringLiteral("<span style=\"color:%1\"> · %2</span>").arg(noteColour, reloadNote.toHtmlEscaped());
    } else if (m_session && m_server->state() == DevServer::State::Failed) {
        text += tr(" — saving a fix starts it again");
    }
    if (!rich)
        text = m_text->fontMetrics().elidedText(text, Qt::ElideRight, 240);
    m_dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(colour));
    m_text->setTextFormat(rich ? Qt::RichText : Qt::PlainText);
    m_text->setText(text);
    if (m_server->state() == DevServer::State::Running && m_server->port() > 0) {
        m_text->setToolTip(tr("Running on %1 — click to open in the browser").arg(m_server->url()));
        m_text->setCursor(Qt::PointingHandCursor);
    } else {
        m_text->setToolTip(m_project.valid ? command() : QString());
        m_text->setCursor(Qt::ArrowCursor);
    }

    const bool active = m_server->isActive();
    m_toggle->setIcon(Icons::tinted(active ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/play.svg"),
                                    QColor(active ? red : green)));
    m_toggle->setToolTip(active ? tr("Stop the Ktor server")
                                : tr("Start the Ktor server (%1)").arg(m_project.valid ? command() : QString()));
    m_restart->setEnabled(m_server->state() != DevServer::State::Stopped);

    const KtorProject::Options o = options();
    const bool custom = !o.command.trimmed().isEmpty();
    m_reload->setChecked(o.autoReload && !custom);
    m_reload->setEnabled(m_project.valid && !custom);
    QString tip;
    if (custom)
        tip = tr("Auto-reload is not available with a custom command");
    else if (!o.autoReload)
        tip = tr("Auto-reload is off. Turn it on to compile every change and reload the running server");
    else if (m_session && !restartReason().isEmpty())
        tip = tr("Auto-reload is on: every change is compiled and the server restarted (%1)").arg(restartReason());
    else
        tip = tr("Auto-reload is on: every change is compiled and Ktor reloads it in place (development mode)");
    if (m_session && m_reloader) {
        if (m_reloader->state() == KtorReloader::State::Failed && !m_reloader->errors().isEmpty())
            tip += QLatin1Char('\n') + m_reloader->errors().mid(0, 6).join(QLatin1Char('\n'));
        else if (!m_reloader->failure().isEmpty())
            tip += QLatin1Char('\n') + tr("Stopped: %1").arg(m_reloader->failure());
        else if (m_server->lastReloadSeconds() > 0)
            tip += QLatin1Char('\n') + tr("Last reload took %1 s").arg(m_server->lastReloadSeconds(), 0, 'f', 2);
    }
    m_reload->setToolTip(tip);
}
