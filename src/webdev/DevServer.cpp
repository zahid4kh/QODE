#include "DevServer.h"

#include "lsp/LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

#include <csignal>
#include <unistd.h>

namespace {

QString stripAnsi(QString s)
{
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]|\x1b\\][^\x07]*\x07"));
    return s.remove(ansi).remove(QLatin1Char('\r'));
}

} // namespace

DevServer::DevServer(QObject *parent) : QObject(parent), m_proc(new QProcess(this)), m_killTimer(new QTimer(this))
{
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    // Own process group (and session): the whole tree can be signalled, and Ctrl+C in a terminal does not reach it.
    m_proc->setChildProcessModifier([] { ::setsid(); });
    connect(m_proc, &QProcess::readyRead, this, &DevServer::onOutput);
    connect(m_proc, &QProcess::finished, this, &DevServer::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_error = tr("Could not start the shell");
            setState(State::Failed);
        }
    });
    m_killTimer->setSingleShot(true);
    m_killTimer->setInterval(4000);
    connect(m_killTimer, &QTimer::timeout, this, [this] { signalGroup(SIGKILL); });
}

DevServer::~DevServer()
{
    if (m_proc->state() != QProcess::NotRunning) {
        signalGroup(SIGTERM);
        if (!m_proc->waitForFinished(1500))
            signalGroup(SIGKILL);
        m_proc->waitForFinished(500);
    }
}

QString DevServer::url() const
{
    return m_port > 0 ? QStringLiteral("http://localhost:%1").arg(m_port) : QString();
}

void DevServer::start(const QString &command, const QString &workDir, int port)
{
    if (m_proc->state() != QProcess::NotRunning) {
        // Still shutting down: start as soon as it is gone.
        m_restart = true;
        m_command = command;
        m_workDir = workDir;
        m_wantedPort = port;
        stop();
        return;
    }
    m_command = command;
    m_workDir = workDir;
    m_wantedPort = port;
    m_port = 0;
    m_error.clear();
    m_log.clear();
    m_pending.clear();
    m_stopping = false;
    m_restart = false;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // A desktop-launched QODE lacks the shell's PATH: add Node's folder (nvm, fnm, ...) and the usual bun/pnpm homes.
    QStringList extra;
    const QString node = LspServers::nodeExecutable();
    if (!node.isEmpty())
        extra << QFileInfo(node).absolutePath();
    const QString home = QDir::homePath();
    extra << home + QStringLiteral("/.bun/bin") << home + QStringLiteral("/.local/share/pnpm") << home + QStringLiteral("/.yarn/bin")
          << home + QStringLiteral("/.local/bin") << home + QStringLiteral("/.npm-global/bin");
    env.insert(QStringLiteral("PATH"), extra.join(QLatin1Char(':')) + QLatin1Char(':') + env.value(QStringLiteral("PATH")));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    env.remove(QStringLiteral("FORCE_COLOR"));
    env.insert(QStringLiteral("BROWSER"), QStringLiteral("none")); // create-react-app would otherwise open a tab itself
    if (port > 0)
        env.insert(QStringLiteral("PORT"), QString::number(port));
    m_proc->setProcessEnvironment(env);
    m_proc->setWorkingDirectory(workDir);
    setState(State::Starting);
    m_proc->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
}

void DevServer::stop()
{
    if (m_proc->state() == QProcess::NotRunning) {
        if (m_state != State::Stopped)
            setState(State::Stopped);
        return;
    }
    m_stopping = true;
    signalGroup(SIGTERM);
    m_killTimer->start();
}

void DevServer::restart()
{
    if (m_command.isEmpty())
        return;
    start(m_command, m_workDir, m_wantedPort);
}

void DevServer::signalGroup(int sig)
{
    const qint64 pid = m_proc->processId();
    if (pid > 0)
        ::kill(-static_cast<pid_t>(pid), sig);
}

void DevServer::onOutput()
{
    const QString text = stripAnsi(QString::fromLocal8Bit(m_proc->readAll()));
    if (text.isEmpty())
        return;
    m_log += text;
    if (m_log.size() > 200000)
        m_log.remove(0, m_log.size() - 150000);
    emit output(text);
    m_pending += text;
    int nl;
    while ((nl = m_pending.indexOf(QLatin1Char('\n'))) >= 0) {
        scan(m_pending.left(nl));
        m_pending.remove(0, nl + 1);
    }
    // A prompt-like line without a newline (some servers print the URL first).
    if (m_state == State::Starting && !m_pending.isEmpty())
        scan(m_pending);
}

void DevServer::scan(const QString &line)
{
    if (m_state != State::Starting)
        return;
    static const QRegularExpression url(QStringLiteral(R"((?:localhost|127\.0\.0\.1|0\.0\.0\.0|\[::1?\]):(\d{2,5}))"));
    static const QRegularExpression listening(QStringLiteral(R"(listening.*?\b(?:port|on)\b\D{0,12}(\d{2,5}))"),
                                              QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = url.match(line);
    if (!m.hasMatch())
        m = listening.match(line);
    if (!m.hasMatch())
        return;
    const int port = m.captured(1).toInt();
    if (port < 1 || port > 65535)
        return;
    m_port = port;
    setState(State::Running);
}

void DevServer::onFinished(int code, QProcess::ExitStatus status)
{
    m_killTimer->stop();
    if (m_restart) {
        start(m_command, m_workDir, m_wantedPort);
        return;
    }
    if (m_stopping || (status == QProcess::NormalExit && code == 0)) {
        setState(State::Stopped);
    } else {
        m_error = status == QProcess::CrashExit ? tr("The server was terminated") : tr("The server exited with code %1").arg(code);
        setState(State::Failed);
    }
    m_stopping = false;
}

void DevServer::setState(State s)
{
    if (s == State::Stopped || s == State::Failed)
        m_port = s == State::Stopped ? 0 : m_port;
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}
