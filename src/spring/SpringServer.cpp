#include "spring/SpringServer.h"

#include <QRegularExpression>

SpringServer::SpringServer(QObject *parent) : DevServer(parent)
{
    connect(this, &DevServer::stateChanged, this, [this] {
        switch (state()) {
        case State::Starting:
            m_startup = 0;
            m_up.invalidate();
            setPhase(Phase::Building);
            break;
        case State::Running:
            setPhase(Phase::Up);
            break;
        case State::Stopped:
        case State::Failed:
            m_up.invalidate();
            setPhase(Phase::Idle);
            break;
        }
    });
}

QString SpringServer::phaseText() const
{
    return m_phase == Phase::Building ? tr("Building…") : tr("Starting…");
}

qint64 SpringServer::uptimeMs() const
{
    return m_up.isValid() ? m_up.elapsed() : 0;
}

void SpringServer::setPhase(Phase p)
{
    if (m_phase == p)
        return;
    m_phase = p;
    emit stateChanged(); // the bar and panel show the phase text
}

void SpringServer::adjustEnvironment(QProcessEnvironment &env)
{
    env.insert(QStringLiteral("SPRING_OUTPUT_ANSI_ENABLED"), QStringLiteral("never"));
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    env.remove(QStringLiteral("PORT")); // Spring does not read it; a leftover value would only mislead
    for (const QString &e : std::as_const(m_extraEnv)) {
        const int eq = e.indexOf(QLatin1Char('='));
        env.insert(e.left(eq).trimmed(), e.mid(eq + 1).trimmed());
    }
}

void SpringServer::scan(const QString &line)
{
    static const QRegularExpression startedLine(QStringLiteral(R"(Started \S+ in ([\d.,]+) seconds)"));
    if (m_state == State::Running && m_startup <= 0) {
        // "Started XApplication in 2.3 seconds" follows the "started on port" line by a few milliseconds.
        const QRegularExpressionMatch sm = startedLine.match(line);
        if (sm.hasMatch()) {
            QString secs = sm.captured(1);
            secs.replace(QLatin1Char(','), QLatin1Char('.'));
            m_startup = secs.toDouble();
            emit stateChanged();
        }
        return;
    }
    if (m_state != State::Starting)
        return;

    static const QRegularExpression runTask(QStringLiteral(R"(^> Task :(?:[\w-]+:)*bootRun\b|--- .*spring-boot.*:run|Running with Spring Boot)"));
    static const QRegularExpression portInUse(QStringLiteral(R"(Port (\d+) was already in use)"));
    static const QRegularExpression portBind(QStringLiteral(R"((?:Address already in use|BindException).*)"));
    static const QRegularExpression buildFailed(QStringLiteral(R"(^FAILURE: Build failed|^\[ERROR\] .*BUILD FAILURE|^BUILD FAILURE|Compilation error|^e: )"));
    static const QRegularExpression webStarted(QStringLiteral(R"((?:Tomcat|Jetty|Undertow|Netty|Reactor Netty|Grizzly)[^\n]*?started on ports?\(?s?\)?:?\s*(\d{2,5}))"),
                                               QRegularExpression::CaseInsensitiveOption);

    if (m_phase == Phase::Building && runTask.match(line).hasMatch())
        setPhase(Phase::Starting);
    // The application's own banner or first log line also proves the build is over (Maven, custom commands).
    if (m_phase == Phase::Building && line.contains(QStringLiteral(" --- ")) && line.contains(QStringLiteral("main]")))
        setPhase(Phase::Starting);

    QRegularExpressionMatch m = portInUse.match(line);
    if (m.hasMatch()) {
        m_error = tr("Port %1 is already in use").arg(m.captured(1));
        return;
    }
    if (m_error.isEmpty() && portBind.match(line).hasMatch()) {
        m_error = tr("The port is already in use");
        return;
    }
    if (m_error.isEmpty() && buildFailed.match(line).hasMatch() && m_phase == Phase::Building) {
        m_error = tr("The build failed — see the log");
        return;
    }

    m = webStarted.match(line);
    if (m.hasMatch()) {
        const int port = m.captured(1).toInt();
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
        if (!m_up.isValid())
            m_up.start();
        setState(State::Running); // no embedded web server (a batch or messaging application): port stays 0
    }
}
