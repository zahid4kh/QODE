#include "DevServerBar.h"

#include "DevServer.h"
#include "ServerDialogs.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QToolButton>
#include <QUrl>

DevServerBar::DevServerBar(QWidget *parent)
    : QWidget(parent), m_server(new DevServer(this)), m_watcher(new QFileSystemWatcher(this)), m_redetect(new QTimer(this))
{
    m_redetect->setSingleShot(true);
    m_redetect->setInterval(500);
    connect(m_redetect, &QTimer::timeout, this, &DevServerBar::redetect);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_redetect, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::fileChanged, m_redetect, qOverload<>(&QTimer::start));
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(4, 0, 4, 0);
    l->setSpacing(2);
    m_dot = new QLabel(this);
    m_dot->setFixedSize(8, 8);
    m_text = new QLabel(this);
    m_text->setContentsMargins(4, 0, 6, 0);
    m_text->setCursor(Qt::ArrowCursor);
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
    m_restart = button(tr("Restart the dev server"));
    m_log = button(tr("Show the dev server log"));
    m_config = button(tr("Dev server settings (package manager, script, port)"));
    Icons::bind(m_restart, QStringLiteral(":/new-icons/refresh-cw.svg"));
    Icons::bind(m_log, QStringLiteral(":/new-icons/scroll-text.svg"));
    Icons::bind(m_config, QStringLiteral(":/new-icons/cog.svg"));

    connect(m_toggle, &QToolButton::clicked, this, &DevServerBar::toggle);
    connect(m_restart, &QToolButton::clicked, this, &DevServerBar::restart);
    connect(m_log, &QToolButton::clicked, this, &DevServerBar::showLog);
    connect(m_config, &QToolButton::clicked, this, &DevServerBar::configure);
    connect(m_text, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    connect(m_server, &DevServer::stateChanged, this, [this] {
        refresh();
        emit stateChanged();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refresh(); });
    refresh();
}

bool DevServerBar::isActive() const
{
    return m_server && m_server->isActive();
}

void DevServerBar::setProjectRoot(const QString &root)
{
    if (m_server)
        m_server->stop();
    m_root = root;
    redetect();
}

void DevServerBar::redetect()
{
    const bool was = m_project.valid;
    m_project = WebProject::detect(m_root);
    // The root folder catches a package.json or lockfile appearing; the files catch edits (editors may replace them).
    const QStringList watched = m_watcher->files() + m_watcher->directories();
    if (!watched.isEmpty())
        m_watcher->removePaths(watched);
    if (!m_root.isEmpty()) {
        m_watcher->addPath(m_root);
        for (const char *f : {"package.json", "bun.lock", "bun.lockb", "pnpm-lock.yaml", "yarn.lock", "package-lock.json"})
            if (QFileInfo::exists(m_root + QLatin1Char('/') + QLatin1String(f)))
                m_watcher->addPath(m_root + QLatin1Char('/') + QLatin1String(f));
    }
    refresh();
    if (was != m_project.valid)
        emit availabilityChanged(m_project.valid);
}

DevServerBar::Effective DevServerBar::effective() const
{
    const QJsonObject cfg = SettingsManager::instance().webServer();
    Effective e;
    e.manager = cfg.value(QStringLiteral("manager")).toString();
    if (!WebProject::managers().contains(e.manager))
        e.manager = m_project.packageManager;
    e.script = cfg.value(QStringLiteral("script")).toString();
    if (e.script.isEmpty())
        e.script = m_project.scripts.value(0);
    e.port = cfg.value(QStringLiteral("port")).toInt();
    e.command = cfg.value(QStringLiteral("command")).toString().trimmed();
    if (e.command.isEmpty())
        e.command = m_project.command(e.manager, e.script, e.port);
    return e;
}

void DevServerBar::start()
{
    if (!m_server)
        return;
    if (!m_server->isActive())
        redetect(); // pick up a changed package.json / lockfile
    if (!m_server || !m_project.valid)
        return;
    const Effective e = effective();
    m_server->start(e.command, m_root, e.port);
}

void DevServerBar::stop()
{
    if (m_server)
        m_server->stop();
}

void DevServerBar::restart()
{
    if (!m_project.valid)
        return;
    start(); // start() on a live server stops it first and starts again with the current settings
}

void DevServerBar::toggle()
{
    if (m_server && m_server->isActive())
        stop();
    else
        start();
}

void DevServerBar::shutdown()
{
    delete m_server; // the destructor ends the process group
    m_server = nullptr;
}

void DevServerBar::showLog()
{
    if (!m_server)
        return;
    if (!m_logDialog) {
        m_logDialog = new ServerLogDialog(m_server, window());
        m_logDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_logDialog, &QObject::destroyed, this, [this] { m_logDialog = nullptr; });
    }
    m_logDialog->show();
    m_logDialog->raise();
    m_logDialog->activateWindow();
}

void DevServerBar::configure()
{
    if (!m_project.valid)
        return;
    ServerConfigDialog dlg(m_project, SettingsManager::instance().webServer(), window());
    if (dlg.exec() != QDialog::Accepted)
        return;
    SettingsManager::instance().setWebServer(dlg.config());
    refresh();
    if (m_server->isActive())
        restart(); // apply the new port / command right away
}

void DevServerBar::refresh()
{
    if (!m_server)
        return;
    const bool dark = SettingsManager::instance().theme() != QLatin1String("light");
    const QString green = dark ? QStringLiteral("#4ec969") : QStringLiteral("#1a8f3c");
    const QString red = dark ? QStringLiteral("#f26b6b") : QStringLiteral("#c42b2b");
    const QString amber = dark ? QStringLiteral("#e5b94e") : QStringLiteral("#b07d0a");
    const QString grey = dark ? QStringLiteral("#6b7078") : QStringLiteral("#a0a4ab");

    QString colour = grey, text;
    switch (m_server->state()) {
    case DevServer::State::Stopped:
        text = tr("Server stopped");
        break;
    case DevServer::State::Starting:
        colour = amber;
        text = tr("Starting…");
        break;
    case DevServer::State::Running: {
        colour = green;
        text = QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">localhost:%3</a>")
                   .arg(m_server->url(), green).arg(m_server->port());
        break;
    }
    case DevServer::State::Failed:
        colour = red;
        text = m_server->errorString().toHtmlEscaped();
        break;
    }
    m_dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(colour));
    m_text->setTextFormat(m_server->state() == DevServer::State::Running ? Qt::RichText : Qt::PlainText);
    m_text->setText(text);
    if (m_server->state() == DevServer::State::Running) {
        m_text->setToolTip(tr("Running on %1 — click to open in the browser").arg(m_server->url()));
        m_text->setCursor(Qt::PointingHandCursor);
    } else {
        m_text->setToolTip(m_project.valid ? effective().command : QString());
        m_text->setCursor(Qt::ArrowCursor);
    }

    const bool active = m_server->isActive();
    m_toggle->setIcon(Icons::tinted(active ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/play.svg"),
                                    QColor(active ? red : green)));
    m_toggle->setToolTip(active ? tr("Stop the dev server") : tr("Start the dev server (%1)").arg(m_project.valid ? effective().command : QString()));
    m_restart->setEnabled(m_server->state() != DevServer::State::Stopped);
}
