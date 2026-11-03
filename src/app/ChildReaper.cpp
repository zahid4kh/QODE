#include "app/ChildReaper.h"

#include <QCoreApplication>
#include <QProcess>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

const char kFlag[] = "--qode-reaper";

// The write end of the pipe to the watchdog, -1 when it could not be started.
int g_fd = -2;

int reaperFd()
{
    if (g_fd != -2)
        return g_fd;
    g_fd = -1;
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0)
        return -1;
    const QByteArray exe = QCoreApplication::applicationFilePath().toLocal8Bit();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        // dup2 clears close-on-exec: the read end becomes the watchdog's stdin; the write end closes at exec.
        ::dup2(fds[0], 0);
        const int null = ::open("/dev/null", O_WRONLY);
        if (null >= 0) {
            ::dup2(null, 1);
            ::dup2(null, 2);
        }
        ::setsid(); // out of QODE's process group: a Ctrl+C or a group kill meant for QODE does not reach it
        char *argv[] = {const_cast<char *>(exe.constData()), const_cast<char *>(kFlag), nullptr};
        ::execv(exe.constData(), argv);
        ::_exit(127);
    }
    ::close(fds[0]);
    g_fd = fds[1];
    return g_fd;
}

void send(char op, qint64 pgid)
{
    const int fd = reaperFd();
    if (fd < 0 || pgid <= 0)
        return;
    const std::string line = op + std::to_string(pgid) + '\n';
    // One small write() is atomic and synchronous: the group is known to the watchdog before QODE could die.
    while (::write(fd, line.data(), line.size()) < 0 && errno == EINTR) {
    }
}

bool groupAlive(pid_t pgid)
{
    return ::kill(-pgid, 0) == 0 || errno == EPERM;
}

} // namespace

bool ChildReaper::runIfReaper(int argc, char *argv[])
{
    if (argc < 2 || std::strcmp(argv[1], kFlag) != 0)
        return false;
    ::prctl(PR_SET_NAME, "qode-reaper", 0, 0, 0);
    std::signal(SIGTERM, SIG_IGN);
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);

    std::set<pid_t> groups;
    std::string pending;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; // QODE is gone (or closed the pipe at a normal exit)
        pending.append(buf, size_t(n));
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (line.size() < 2)
                continue;
            const pid_t pgid = pid_t(std::atol(line.c_str() + 1));
            if (pgid <= 1)
                continue;
            if (line[0] == '+')
                groups.insert(pgid);
            else
                groups.erase(pgid);
        }
    }

    for (const pid_t g : groups)
        ::kill(-g, SIGTERM);
    // Gradle and Spring Boot stop within a second or two on SIGTERM; give them 3 s, then kill what is left.
    for (int i = 0; i < 30 && !groups.empty(); ++i) {
        for (auto it = groups.begin(); it != groups.end();)
            it = groupAlive(*it) ? std::next(it) : groups.erase(it);
        if (!groups.empty())
            ::usleep(100 * 1000);
    }
    for (const pid_t g : groups)
        ::kill(-g, SIGKILL);
    return true;
}

void ChildReaper::track(QProcess *process)
{
    process->setChildProcessModifier([] { ::setsid(); });
    // The group id is the leader's pid; processId() is 0 again once it finished, so it is kept here.
    auto *pgid = new qint64(0);
    QObject::connect(process, &QProcess::started, process, [process, pgid] {
        *pgid = process->processId();
        send('+', *pgid);
    });
    QObject::connect(process, &QProcess::stateChanged, process, [pgid](QProcess::ProcessState s) {
        if (s == QProcess::NotRunning && *pgid > 0) {
            send('-', *pgid);
            *pgid = 0;
        }
    });
    QObject::connect(process, &QObject::destroyed, [pgid] {
        if (*pgid > 0)
            send('-', *pgid);
        delete pgid;
    });
}
