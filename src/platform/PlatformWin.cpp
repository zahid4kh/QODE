#include "platform/Platform.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QtGlobal>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

namespace {

qint64 fileTimeTo100ns(const FILETIME &t)
{
    return (qint64(t.dwHighDateTime) << 32) | qint64(t.dwLowDateTime);
}

const char kRestartArg[] = "--qode-restart-after";

std::wstring ownExe()
{
    wchar_t buf[MAX_PATH * 4];
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return len > 0 && len < std::size(buf) ? std::wstring(buf, len) : std::wstring();
}

// Starts this binary with `args` (already quoted as needed), with no console and not waited for.
bool startOwnExe(const std::wstring &args)
{
    const std::wstring exe = ownExe();
    if (exe.empty())
        return false;
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? std::wstring() : L" " + args);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool Platform::runIfHelper(int argc, char *argv[])
{
    // restartSelf(): wait (at most 30 s) until the old QODE has exited and released its project, then start QODE.
    if (argc != 3 || std::strcmp(argv[1], kRestartArg) != 0)
        return false;
    const DWORD pid = DWORD(std::strtoul(argv[2], nullptr, 10));
    if (const HANDLE old = pid > 0 ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr) {
        ::WaitForSingleObject(old, 30000);
        ::CloseHandle(old);
    }
    startOwnExe({});
    return true;
}

void Platform::initProcess()
{
}

qint64 Platform::msBeforeMain()
{
    FILETIME created, exited, kernel, user, now;
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return -1;
    ::GetSystemTimePreciseAsFileTime(&now);
    const qint64 diff = fileTimeTo100ns(now) - fileTimeTo100ns(created);
    return diff >= 0 ? diff / 10000 : -1;
}

QString Platform::userKey()
{
    // The SID ("S-1-5-21-…") is unique per user and never changes, unlike the user name.
    QString key;
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size > 0) {
            QByteArray buf(int(size), '\0');
            if (::GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
                LPWSTR sid = nullptr;
                if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &sid)) {
                    key = QString::fromWCharArray(sid);
                    ::LocalFree(sid);
                }
            }
        }
        ::CloseHandle(token);
    }
    if (key.isEmpty())
        key = qEnvironmentVariable("USERNAME", QStringLiteral("user"));
    return key;
}

bool Platform::replaceSymlink(const QString &, const QString &)
{
    return false;
}

void Platform::setShellCommand(QProcess *process, const QString &command)
{
    // /d: no AutoRun commands from the registry. /s /c "<line>": cmd drops the outer quotes and runs the rest as it is,
    // so the line keeps its own quotes. QProcess would quote each argument for the C runtime, which cmd does not parse,
    // so the line goes in as native arguments.
    process->setProgram(qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe")));
    process->setArguments({});
    process->setNativeArguments(QStringLiteral("/d /s /c \"") + command + QLatin1Char('"'));
}

namespace {

// One argument for a cmd.exe command line: quoted when it holds a space or a character cmd treats specially, with
// embedded quotes doubled (what batch files expect).
QString cmdQuote(const QString &arg)
{
    if (arg.isEmpty())
        return QStringLiteral("\"\"");
    static const QString special = QStringLiteral(" \t\"&|<>^(),;=");
    bool plain = true;
    for (const QChar c : arg)
        if (special.contains(c)) {
            plain = false;
            break;
        }
    if (plain)
        return arg;
    QString q = arg;
    q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

} // namespace

void Platform::setCommand(QProcess *process, const QString &program, const QStringList &args)
{
    QString exe = program;
    if (!exe.contains(QLatin1Char('/')) && !exe.contains(QLatin1Char('\\'))) {
        const QString found = QStandardPaths::findExecutable(exe); // adds PATHEXT: gradle -> gradle.bat
        if (!found.isEmpty())
            exe = found;
    }
    const QString suffix = QFileInfo(exe).suffix().toLower();
    if (suffix != QLatin1String("bat") && suffix != QLatin1String("cmd")) {
        process->setProgram(exe);
        process->setArguments(args);
        return;
    }
    QString line = cmdQuote(QDir::toNativeSeparators(exe));
    for (const QString &a : args)
        line += QLatin1Char(' ') + cmdQuote(a);
    setShellCommand(process, line);
}

QString Platform::wrapperCommand(const QString &dir, const QString &name)
{
    return wrapperFile(dir, name).isEmpty() ? QString() : QStringLiteral(".\\") + name;
}

QString Platform::wrapperFile(const QString &dir, const QString &name)
{
    for (const char *ext : {".bat", ".cmd"}) {
        const QString path = dir + QLatin1Char('/') + name + QLatin1String(ext);
        if (QFileInfo(path).isFile())
            return path;
    }
    return {};
}

bool Platform::wrapperNeedsExecBit(const QString &, const QString &)
{
    return false;
}

QString Platform::venvBinDir()
{
    return QStringLiteral("Scripts");
}

QString Platform::venvProgram(const QString &venv, const QString &name)
{
    const QFileInfo fi(venv + QStringLiteral("/Scripts/") + name + QStringLiteral(".exe"));
    return fi.isFile() ? fi.absoluteFilePath() : QString();
}

QString Platform::gitExecutable()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (!onPath.isEmpty())
        return onPath;
    // Git for Windows: all users (Program Files), or per user (LocalAppData\Programs).
    QStringList roots{qEnvironmentVariable("ProgramW6432"), qEnvironmentVariable("ProgramFiles")};
    if (const QString local = qEnvironmentVariable("LOCALAPPDATA"); !local.isEmpty())
        roots << local + QStringLiteral("\\Programs");
    for (const QString &root : std::as_const(roots)) {
        if (root.isEmpty())
            continue;
        const QString git = QDir::fromNativeSeparators(root) + QStringLiteral("/Git/cmd/git.exe");
        if (QFileInfo(git).isFile())
            return git;
    }
    return {};
}

QStringList Platform::extraBinDirs()
{
    QStringList out;
    const auto add = [&out](const QString &dir) {
        if (!dir.isEmpty() && QFileInfo(dir).isDir())
            out << QDir::toNativeSeparators(dir);
    };
    const QString appData = qEnvironmentVariable("APPDATA"), local = qEnvironmentVariable("LOCALAPPDATA");
    const QString programFiles = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:\\Program Files"));
    if (!appData.isEmpty())
        add(appData + QStringLiteral("\\npm")); // npm install -g
    if (!local.isEmpty()) {
        add(local + QStringLiteral("\\pnpm"));
        add(local + QStringLiteral("\\Microsoft\\WinGet\\Links"));
        add(local + QStringLiteral("\\Volta\\bin"));
    }
    add(programFiles + QStringLiteral("\\nodejs"));
    add(QDir::homePath() + QStringLiteral("/scoop/shims"));
    return out;
}

void Platform::restartSelf()
{
    startOwnExe(QString::fromLatin1(kRestartArg).toStdWString() + L" " + std::to_wstring(::GetCurrentProcessId()));
}
