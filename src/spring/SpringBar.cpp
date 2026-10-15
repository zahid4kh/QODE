#include "spring/SpringBar.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "spring/SpringConfigDialog.h"
#include "spring/SpringServer.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

SpringBar::SpringBar(QWidget *parent)
    : QWidget(parent), m_server(new SpringServer(this)), m_watcher(new QFileSystemWatcher(this)), m_redetect(new QTimer(this))
{
    m_redetect->setSingleShot(true);
    m_redetect->setInterval(500);
    connect(m_redetect, &QTimer::timeout, this, &SpringBar::redetect);
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
    m_restart = button(tr("Restart the Spring Boot application"));
    m_panel = button(tr("Show the Spring Boot panel (status and live log)"));
    m_config = button(tr("Spring Boot settings (profiles, port, arguments)"));
    Icons::bind(m_restart, QStringLiteral(":/new-icons/refresh-cw.svg"));
    Icons::bind(m_panel, QStringLiteral(":/new-icons/springboot.svg"));
    Icons::bind(m_config, QStringLiteral(":/new-icons/cog.svg"));

    connect(m_toggle, &QToolButton::clicked, this, &SpringBar::toggle);
    connect(m_restart, &QToolButton::clicked, this, &SpringBar::restart);
    connect(m_panel, &QToolButton::clicked, this, &SpringBar::panelRequested);
    connect(m_config, &QToolButton::clicked, this, &SpringBar::configure);
    connect(m_text, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    connect(m_server, &DevServer::stateChanged, this, [this] {
        refresh();
        emit stateChanged();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refresh(); });
    refresh();
}

bool SpringBar::isActive() const
{
    return m_server && m_server->isActive();
}

SpringProject::Options SpringBar::options() const
{
    const QJsonObject cfg = SettingsManager::instance().springServer();
    SpringProject::Options o;
    o.profiles = cfg.value(QStringLiteral("profiles")).toString();
    o.port = cfg.value(QStringLiteral("port")).toInt();
    o.arguments = cfg.value(QStringLiteral("arguments")).toString();
    o.command = cfg.value(QStringLiteral("command")).toString();
    return o;
}

QString SpringBar::command() const
{
    return m_project.command(options());
}

void SpringBar::setProjectRoot(const QString &root)
{
    if (m_server)
        m_server->stop();
    const bool changed = root != m_root;
    m_root = root;
    redetect();
    if (changed)
        emit rootChanged();
}

void SpringBar::redetect()
{
    const bool was = isAvailable();
    m_project = SpringProject::detect(m_root);
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
}

void SpringBar::start()
{
    if (!m_server)
        return;
    if (!m_server->isActive())
        redetect(); // pick up a changed build file
    if (!m_server || !isAvailable())
        return;
    emit starting();
    m_server->start(command(), m_project.workDir, 0);
}

void SpringBar::stop()
{
    if (m_server)
        m_server->stop();
}

void SpringBar::restart()
{
    if (m_project.valid)
        start(); // start() on a live server stops it first and starts again with the current settings
}

void SpringBar::toggle()
{
    if (isActive())
        stop();
    else
        start();
}

void SpringBar::shutdown()
{
    delete m_server; // the destructor ends the process group
    m_server = nullptr;
}

void SpringBar::configure()
{
    if (!m_project.valid)
        return;
    SpringConfigDialog dlg(m_project, options(), window());
    if (dlg.exec() != QDialog::Accepted)
        return;
    const SpringProject::Options o = dlg.options();
    QJsonObject cfg;
    if (!o.profiles.isEmpty())
        cfg.insert(QStringLiteral("profiles"), o.profiles);
    if (o.port > 0)
        cfg.insert(QStringLiteral("port"), o.port);
    if (!o.arguments.isEmpty())
        cfg.insert(QStringLiteral("arguments"), o.arguments);
    if (!o.command.isEmpty())
        cfg.insert(QStringLiteral("command"), o.command);
    SettingsManager::instance().setSpringServer(cfg);
    refresh();
    if (isActive())
        restart(); // apply the new profile / port right away
}

void SpringBar::refresh()
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
        text = tr("Spring stopped");
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
    m_toggle->setToolTip(active ? tr("Stop the Spring Boot application")
                                : tr("Start the Spring Boot application (%1)").arg(m_project.valid ? command() : QString()));
    m_restart->setEnabled(m_server->state() != DevServer::State::Stopped);
}
