#include "platform/Platform.h"

#include <QtGlobal>

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

} // namespace

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
