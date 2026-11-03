#include "ktor/KtorReloader.h"

#include "app/ChildReaper.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

#include <csignal>
#include <unistd.h>

KtorReloader::KtorReloader(QObject *parent)
    : QObject(parent), m_proc(new QProcess(this)), m_killTimer(new QTimer(this)), m_debounce(new QTimer(this))
{
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    ChildReaper::track(m_proc); // own process group, stopped by the watchdog if QODE ends abruptly
    connect(m_proc, &QProcess::readyRead, this, &KtorReloader::onOutput);
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_killTimer->stop();
        if (!m_pending.isEmpty()) {
            scan(m_pending);
            m_pending.clear();
        }
        if (m_stopping) {
            m_stopping = false;
            return;
        }
        if (m_project.maven) {
            // One `mvn compile` per change; a change saved meanwhile compiles again.
            finishCycle(status == QProcess::NormalExit && code == 0, true);
            if (m_again && m_on) {
                m_again = false;
                compileSoon();
            }
            return;
        }
        // The continuous build only ends by itself when Gradle cannot go on (a broken build script, a killed daemon).
        m_on = false;
        m_failure = status == QProcess::CrashExit ? tr("the continuous build was terminated") : tr("the continuous build ended (exit code %1)").arg(code);
        setState(State::Off);
    });
    m_killTimer->setSingleShot(true);
    m_killTimer->setInterval(3000);
    connect(m_killTimer, &QTimer::timeout, this, [this] { signalGroup(SIGKILL); });
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        if (!m_on)
            return;
        if (m_proc->state() != QProcess::NotRunning) {
            m_again = true;
            return;
        }
        QStringList args;
        if (m_project.moduleDir != m_project.workDir)
            args << QStringLiteral("-f") << QDir(m_project.workDir).relativeFilePath(m_project.moduleDir) + QStringLiteral("/pom.xml");
        args << QStringLiteral("-q") << QStringLiteral("compile");
        m_cycleErrors.clear();
        setState(State::Compiling);
        launch(args);
    });
}

KtorReloader::~KtorReloader()
{
    if (m_proc->state() != QProcess::NotRunning) {
        m_stopping = true;
        signalGroup(SIGTERM);
        if (!m_proc->waitForFinished(1500))
            signalGroup(SIGKILL);
        m_proc->waitForFinished(500);
    }
}

void KtorReloader::start(const KtorProject &project, const QStringList &extraEnv)
{
    if (m_on)
        return;
    m_project = project;
    m_env = extraEnv;
    m_on = true;
    m_again = false;
    m_failure.clear();
    m_errors.clear();
    if (project.maven) {
        setState(State::Watching);
        return;
    }
    // -t: Gradle watches the inputs of `classes` (sources and resources) and builds again on every change. Its stdin
    // stays open (QProcess keeps the pipe): a continuous build ends at end of input.
    const QString task = project.moduleDir != project.workDir ? project.gradlePath() + QStringLiteral(":classes") : QStringLiteral("classes");
    m_cycleFailed = false;
    m_cycleExecuted = 0;
    m_cycleErrors.clear();
    setState(State::Compiling); // the first round compiles what changed since the server's own build, if anything
    launch({QStringLiteral("--console=plain"), QStringLiteral("-t"), task});
}

void KtorReloader::launch(const QStringList &args)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    for (const QString &e : std::as_const(m_env)) {
        const int eq = e.indexOf(QLatin1Char('='));
        env.insert(e.left(eq).trimmed(), e.mid(eq + 1).trimmed());
    }
    m_proc->setProcessEnvironment(env);
    m_proc->setWorkingDirectory(m_project.workDir);
    m_pending.clear();
    m_stopping = false;
    // The build tool as the server's command names it: "./gradlew" / "./mvnw" relative to the work folder.
    m_proc->start(m_project.tool.startsWith(QLatin1String("./")) ? m_project.workDir + m_project.tool.mid(1) : m_project.tool, args);
}

void KtorReloader::stop()
{
    m_debounce->stop();
    m_again = false;
    const bool was = m_on || m_state != State::Off;
    m_on = false;
    m_failure.clear();
    if (m_proc->state() != QProcess::NotRunning) {
        m_stopping = true;
        signalGroup(SIGTERM);
        m_killTimer->start();
    }
    if (was)
        setState(State::Off);
}

void KtorReloader::compileSoon()
{
    if (m_on && m_project.maven)
        m_debounce->start();
}

void KtorReloader::signalGroup(int sig)
{
    const qint64 pid = m_proc->processId();
    if (pid > 0)
        ::kill(-static_cast<pid_t>(pid), sig);
}

void KtorReloader::onOutput()
{
    m_pending += QString::fromLocal8Bit(m_proc->readAll());
    int nl;
    while ((nl = m_pending.indexOf(QLatin1Char('\n'))) >= 0) {
        QString line = m_pending.left(nl);
        m_pending.remove(0, nl + 1);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        scan(line);
    }
}

void KtorReloader::scan(const QString &line)
{
    // Kotlin: "e: file:///home/me/app/src/main/kotlin/Routing.kt:12:5 Unresolved reference 'x'."
    // Maven's Kotlin plugin: "[ERROR] /home/me/app/src/main/kotlin/Routing.kt: (12, 5) Unresolved reference 'x'."
    // javac: "/home/me/app/src/main/java/A.java:12: error: cannot find symbol"
    static const QRegularExpression kotlinError(QStringLiteral(R"(^e: (?:file://)?(\S+?):(\d+):\d+ (.*))"));
    static const QRegularExpression mavenError(QStringLiteral(R"(^\[ERROR\] (?:file://)?(/\S+?): \((\d+), \d+\) (.*))"));
    static const QRegularExpression javacError(QStringLiteral(R"(^(/\S+\.java):(\d+): error: (.*))"));
    for (const QRegularExpression *re : {&kotlinError, &mavenError, &javacError}) {
        const QRegularExpressionMatch m = re->match(line);
        if (m.hasMatch()) {
            m_cycleErrors << QStringLiteral("%1:%2: %3").arg(QFileInfo(m.captured(1)).fileName(), m.captured(2), m.captured(3));
            emit output(line + QLatin1Char('\n'));
            return;
        }
    }
    if (m_project.maven)
        return;

    static const QRegularExpression changed(QStringLiteral(R"(^(?:Change detected, executing build|modified: |new file: |deleted: ))"));
    static const QRegularExpression executed(QStringLiteral(R"(^\d+ actionable tasks?: (\d+) executed)"));
    if (changed.match(line).hasMatch()) {
        if (m_state != State::Compiling) {
            m_cycleFailed = false;
            m_cycleExecuted = 0;
            m_cycleErrors.clear();
            setState(State::Compiling);
        }
    } else if (line.startsWith(QLatin1String("BUILD FAILED")) || line.startsWith(QLatin1String("FAILURE: Build failed"))) {
        m_cycleFailed = true;
    } else if (const QRegularExpressionMatch m = executed.match(line); m.hasMatch()) {
        m_cycleExecuted = m.captured(1).toInt();
    } else if (line.startsWith(QLatin1String("Waiting for changes to input files"))) {
        finishCycle(!m_cycleFailed, m_cycleExecuted > 0);
    }
}

void KtorReloader::finishCycle(bool ok, bool wroteClasses)
{
    if (!m_on)
        return;
    if (!ok) {
        m_errors = m_cycleErrors.isEmpty() ? QStringList{tr("The build failed — see the log")} : m_cycleErrors;
        m_cycleErrors.clear();
        setState(State::Failed);
        return;
    }
    m_errors.clear();
    m_cycleErrors.clear();
    setState(State::Watching);
    if (wroteClasses)
        emit compiled();
}

void KtorReloader::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}
