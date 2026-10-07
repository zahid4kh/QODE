#include "ShellProcess.h"

#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

ShellProcess::ShellProcess(QObject *parent)
    : QObject(parent)
{
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::finished, this, &ShellProcess::onProcessFinished);

    m_killTimer = new QTimer(this);
    m_killTimer->setSingleShot(true);
    m_killTimer->setInterval(1500);
    connect(m_killTimer, &QTimer::timeout, this, [this] {
        if (m_process->state() != QProcess::NotRunning)
            m_process->kill();
    });
}

ShellProcess::~ShellProcess()
{
    disconnect(m_process, nullptr, this, nullptr);
    closeMaster();
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(300))
            m_process->kill();
        m_process->waitForFinished(300);
    }
}

QString ShellProcess::defaultShell()
{
    const QString env = qEnvironmentVariable("SHELL");
    for (const QString &candidate : {env, QStringLiteral("/bin/bash"), QStringLiteral("/bin/sh")}) {
        const QFileInfo fi(candidate);
        if (!candidate.isEmpty() && fi.isExecutable() && fi.isFile())
            return candidate;
    }
    return QStringLiteral("/bin/sh");
}

void ShellProcess::setLaunch(const QString &program, const QStringList &args, const QProcessEnvironment &extraEnv)
{
    m_program = program;
    m_args = args;
    m_extraEnv = extraEnv;
}

bool ShellProcess::isRunning() const
{
    return m_process->state() != QProcess::NotRunning;
}

qint64 ShellProcess::pid() const
{
    return m_process->processId();
}

#ifdef Q_OS_UNIX

bool ShellProcess::start(const QString &workingDirectory, int cols, int rows, QString *error)
{
    if (isRunning())
        return true;

    m_master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m_master < 0 || ::grantpt(m_master) != 0 || ::unlockpt(m_master) != 0) {
        if (error)
            *error = QStringLiteral("Could not allocate a pseudo-terminal: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        closeMaster();
        return false;
    }
    char slaveName[128];
    if (::ptsname_r(m_master, slaveName, sizeof slaveName) != 0) {
        if (error)
            *error = QStringLiteral("Could not resolve the pseudo-terminal device.");
        closeMaster();
        return false;
    }
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
    resize(cols, rows);

    m_shell = m_program.isEmpty() ? defaultShell() : m_program;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    env.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("QODE"));
    env.remove(QStringLiteral("COLUMNS"));
    env.remove(QStringLiteral("LINES"));
    env.insert(m_extraEnv);
    m_process->setProcessEnvironment(env);
    m_process->setWorkingDirectory(workingDirectory);
    m_process->setProgram(m_shell);
    m_process->setArguments(m_args.isEmpty() ? QStringList{QStringLiteral("-i")} : m_args);

    // Runs in the forked child just before exec: only async-signal-safe calls allowed.
    const QByteArray slave = QByteArray(slaveName);
    m_process->setChildProcessModifier([slave] {
        ::setsid();
        const int fd = ::open(slave.constData(), O_RDWR);
        if (fd < 0)
            ::_exit(127);
        ::ioctl(fd, TIOCSCTTY, 0);
        ::dup2(fd, 0);
        ::dup2(fd, 1);
        ::dup2(fd, 2);
        if (fd > 2)
            ::close(fd);
    });

    m_process->start(QIODevice::ReadWrite);
    if (!m_process->waitForStarted(3000)) {
        if (error)
            *error = QStringLiteral("Could not start %1: %2").arg(m_shell, m_process->errorString());
        closeMaster();
        return false;
    }

    m_readNotifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &ShellProcess::onReadable);
    m_writeNotifier = new QSocketNotifier(m_master, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &ShellProcess::onWritable);
    return true;
}

void ShellProcess::resize(int cols, int rows)
{
    if (m_master < 0)
        return;
    struct winsize ws {};
    ws.ws_col = quint16(qMax(1, cols));
    ws.ws_row = quint16(qMax(1, rows));
    ::ioctl(m_master, TIOCSWINSZ, &ws);
}

void ShellProcess::onReadable()
{
    char buf[65536];
    while (m_master >= 0) {
        const ssize_t n = ::read(m_master, buf, sizeof buf);
        if (n > 0) {
            emit output(QByteArray(buf, int(n)));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        // EIO / EOF: the slave side closed (shell exited).
        if (m_readNotifier)
            m_readNotifier->setEnabled(false);
        break;
    }
}

void ShellProcess::write(const QByteArray &data)
{
    if (m_master < 0 || data.isEmpty())
        return;
    m_writeBuffer.append(data);
    onWritable();
}

void ShellProcess::onWritable()
{
    while (m_master >= 0 && !m_writeBuffer.isEmpty()) {
        const ssize_t n = ::write(m_master, m_writeBuffer.constData(), size_t(m_writeBuffer.size()));
        if (n > 0) {
            m_writeBuffer.remove(0, int(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break; // EAGAIN: wait for the notifier
        }
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(!m_writeBuffer.isEmpty());
}

void ShellProcess::closeMaster()
{
    delete m_readNotifier;
    delete m_writeNotifier;
    m_readNotifier = m_writeNotifier = nullptr;
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    m_writeBuffer.clear();
}

void ShellProcess::terminate()
{
    if (!isRunning())
        return;
    // Closing the master hangs up the terminal: the kernel sends SIGHUP to the shell and
    // its foreground jobs. Also signal the shell directly in case it was started detached.
    if (m_process->processId() > 0)
        ::kill(pid_t(m_process->processId()), SIGHUP);
    closeMaster();
    m_killTimer->start();
}

#else // !Q_OS_UNIX

bool ShellProcess::start(const QString &, int, int, QString *error)
{
    if (error)
        *error = QStringLiteral("The integrated terminal is only supported on Unix-like systems.");
    return false;
}
void ShellProcess::resize(int, int) {}
void ShellProcess::onReadable() {}
void ShellProcess::write(const QByteArray &) {}
void ShellProcess::onWritable() {}
void ShellProcess::closeMaster() {}
void ShellProcess::terminate() {}

#endif

void ShellProcess::onProcessFinished(int exitCode, QProcess::ExitStatus)
{
    m_killTimer->stop();
    if (m_readNotifier)
        onReadable(); // drain whatever the shell printed last
    closeMaster();
    emit finished(exitCode);
}
