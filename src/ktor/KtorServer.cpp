#include "ktor/KtorServer.h"

#include <QRegularExpression>

KtorServer::KtorServer(QObject *parent) : DevServer(parent)
{
    connect(this, &DevServer::stateChanged, this, [this] {
        switch (state()) {
        case State::Starting:
            m_startup = 0;
            m_development = false;
            m_reloading = false;
            m_debugPort = 0;
            m_lastReload = 0;
            m_reloadBlocker.clear();
            m_up.invalidate();
            setPhase(Phase::Building);
            break;
        case State::Running:
            setPhase(Phase::Up);
            break;
        case State::Stopped:
        case State::Failed:
            m_up.invalidate();
            m_reloading = false;
            m_debugPort = 0;
            setPhase(Phase::Idle);
            break;
        }
    });
}

QString KtorServer::phaseText() const
{
    return m_phase == Phase::Building ? tr("Building…") : tr("Starting…");
}

qint64 KtorServer::uptimeMs() const
{
    return m_up.isValid() ? m_up.elapsed() : 0;
}

void KtorServer::setPhase(Phase p)
{
    if (m_phase == p)
        return;
    m_phase = p;
    emit stateChanged(); // the bar and panel show the phase text
}

void KtorServer::adjustEnvironment(QProcessEnvironment &env)
{
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    // A leftover PORT would be picked up by a "$PORT:8080" in application.yaml and move the server unexpectedly.
    env.remove(QStringLiteral("PORT"));
    for (const QString &e : std::as_const(m_extraEnv)) {
        const int eq = e.indexOf(QLatin1Char('='));
        env.insert(e.left(eq).trimmed(), e.mid(eq + 1).trimmed());
    }
}

// Development mode lines, at start and on every reload (EmbeddedServer.createClassLoader / currentApplication).
void KtorServer::scanReload(const QString &line)
{
    static const QRegularExpression debugLine(QStringLiteral(R"(^Listening for transport dt_socket at address: (\d{1,5}))"));
    static const QRegularExpression reloadedLine(QStringLiteral(R"(Application auto-reloaded in ([\d.,]+) seconds)"));
    if (const QRegularExpressionMatch m = debugLine.match(line); m.hasMatch()) {
        m_debugPort = m.captured(1).toInt(); // printed again, with a new port when it was 0, after each detach
        return;
    }
    if (line.contains(QLatin1String("Autoreload is disabled because the development mode is off")))
        m_reloadBlocker = tr("development mode is off");
    else if (line.contains(QLatin1String("No ktor.deployment.watch patterns specified")))
        m_reloadBlocker = tr("the server has no watch paths (embeddedServer without watchPaths)");
    else if (line.contains(QLatin1String("No ktor.deployment.watch patterns match classpath entries")))
        m_reloadBlocker = tr("the watch paths match nothing on the class path");
    else if (line.contains(QLatin1String("Auto-reload is disabled: application is loaded by")))
        m_reloadBlocker = tr("the application's class loader does not support it");
    if (m_state != State::Running)
        return;
    if (line.contains(QLatin1String("Changes in application detected."))) {
        m_reloading = true;
        emit reloadStarted();
        emit stateChanged();
    } else if (const QRegularExpressionMatch m = reloadedLine.match(line); m.hasMatch()) {
        QString secs = m.captured(1);
        secs.replace(QLatin1Char(','), QLatin1Char('.'));
        m_lastReload = secs.toDouble();
        m_reloading = false;
        emit reloaded(m_lastReload);
        emit stateChanged();
    } else if (line.contains(QLatin1String("Application reload failed.")) || line.contains(QLatin1String("Auto-reload failed."))) {
        if (m_reloading) {
            m_reloading = false;
            emit reloadFailed();
            emit stateChanged();
        }
    }
}

void KtorServer::scan(const QString &line)
{
    static const QRegularExpression startedLine(QStringLiteral(R"(Application started in ([\d.,]+) seconds)"));
    scanReload(line);
    if (m_state == State::Running) {
        // "Application started in 0.2 seconds" can come right after the first "Responding at" line.
        if (m_startup <= 0) {
            const QRegularExpressionMatch sm = startedLine.match(line);
            if (sm.hasMatch()) {
                QString secs = sm.captured(1);
                secs.replace(QLatin1Char(','), QLatin1Char('.'));
                m_startup = secs.toDouble();
                emit stateChanged();
            }
        }
        return;
    }
    if (m_state != State::Starting)
        return;

    static const QRegularExpression runTask(QStringLiteral(R"(^> Task :(?:[\w-]+:)*run\b|--- .*exec.*:java\b)"));
    static const QRegularExpression ktorLine(QStringLiteral(R"(\b(?:Autoreload is|Application started in|Application - |No ktor\.deployment\.watch)"));
    static const QRegularExpression responding(QStringLiteral(R"(Responding at (\w+)://[^\s:/]+:(\d{1,5}))"));
    static const QRegularExpression bind(QStringLiteral(R"(BindException|Address already in use)"));
    static const QRegularExpression buildFailed(QStringLiteral(R"(^FAILURE: Build failed|^\[ERROR\] .*BUILD FAILURE|^BUILD FAILURE|Compilation error|^e: )"));
    static const QRegularExpression noModule(QStringLiteral(R"((?:ClassNotFoundException|Module function cannot be found|NoSuchMethodException)[:\s]+([\w.$]+))"));
    static const QRegularExpression mainException(QStringLiteral(R"(^Exception in thread "main" ([\w.$]+)(?::\s*(.*))?)"));

    if (m_phase == Phase::Building && (runTask.match(line).hasMatch() || ktorLine.match(line).hasMatch()))
        setPhase(Phase::Starting);
    if (line.contains(QStringLiteral("Autoreload is enabled")) || line.contains(QStringLiteral("development mode is on")))
        m_development = true;

    if (m_error.isEmpty() && bind.match(line).hasMatch()) {
        m_error = tr("The port is already in use");
        return;
    }
    if (m_error.isEmpty() && m_phase == Phase::Building && buildFailed.match(line).hasMatch()) {
        m_error = tr("The build failed — see the log");
        return;
    }
    QRegularExpressionMatch m = noModule.match(line);
    if (m_error.isEmpty() && m.hasMatch()) {
        m_error = tr("A module in the configuration was not found: %1").arg(m.captured(1));
        return;
    }
    m = mainException.match(line);
    if (m_error.isEmpty() && m.hasMatch()) {
        const QString type = m.captured(1).section(QLatin1Char('.'), -1);
        m_error = m.captured(2).isEmpty() ? tr("The server failed to start: %1").arg(type) : tr("%1: %2").arg(type, m.captured(2).left(160));
        return;
    }

    m = responding.match(line);
    if (m.hasMatch()) {
        const int port = m.captured(2).toInt();
        if (port > 0 && port <= 65535) {
            m_port = port;
            m_up.start();
            setState(State::Running);
        }
        return;
    }
    m = startedLine.match(line);
    if (m.hasMatch()) {
        QString secs = m.captured(1);
        secs.replace(QLatin1Char(','), QLatin1Char('.'));
        m_startup = secs.toDouble();
    }
}
