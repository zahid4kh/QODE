#include "platform/Platform.h"

#include <QProcess>
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

void Platform::restartSelf()
{
    startOwnExe(QString::fromLatin1(kRestartArg).toStdWString() + L" " + std::to_wstring(::GetCurrentProcessId()));
}
