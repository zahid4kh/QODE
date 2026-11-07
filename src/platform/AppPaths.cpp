#include "platform/AppPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>
#endif

namespace {

const QString kFolder = QStringLiteral("/QODE-AGENTIC");

QString join(const QString &root, const QString &sub)
{
    return sub.isEmpty() ? root : root + QLatin1Char('/') + sub;
}

QString location(QStandardPaths::StandardLocation type)
{
    return QStandardPaths::writableLocation(type);
}

#ifdef Q_OS_WIN
// "<folder of qode.exe>/data" when portable.txt is next to qode.exe, else empty. From the module path, not
// QCoreApplication::applicationDirPath(): AppPaths is also used before the application object exists.
const QString &portableRoot()
{
    static const QString root = [] {
        std::vector<wchar_t> buf(32768);
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0 || n >= buf.size())
            return QString();
        const QString dir = QFileInfo(QDir::fromNativeSeparators(QString::fromWCharArray(buf.data(), int(n)))).absolutePath();
        return QFileInfo::exists(dir + QStringLiteral("/portable.txt")) ? dir + QStringLiteral("/data") : QString();
    }();
    return root;
}
#endif

} // namespace

bool AppPaths::isPortable()
{
#ifdef Q_OS_WIN
    return !portableRoot().isEmpty();
#else
    return false;
#endif
}

QString AppPaths::config(const QString &sub)
{
#ifdef Q_OS_WIN
    if (isPortable())
        return join(portableRoot() + QStringLiteral("/config"), sub);
    // GenericConfigLocation is %LOCALAPPDATA% on Windows; settings belong in the roaming profile.
    QString root = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
    if (root.isEmpty())
        root = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return join(root + kFolder, sub);
#else
    return join(location(QStandardPaths::GenericConfigLocation) + kFolder, sub);
#endif
}

QString AppPaths::data(const QString &sub)
{
#ifdef Q_OS_WIN
    if (isPortable())
        return join(portableRoot() + QStringLiteral("/local"), sub);
#endif
    return join(location(QStandardPaths::GenericDataLocation) + kFolder, sub); // %LOCALAPPDATA% on Windows
}

QString AppPaths::cache(const QString &sub)
{
#ifdef Q_OS_WIN
    // GenericCacheLocation would be %LOCALAPPDATA%\cache: keep everything of QODE's in one folder.
    return join(data(QStringLiteral("cache")), sub);
#else
    return join(location(QStandardPaths::GenericCacheLocation) + kFolder, sub);
#endif
}

QString AppPaths::settingsFile()
{
#ifdef Q_OS_WIN
    return config(QStringLiteral("QODE.ini"));
#else
    return config(QStringLiteral("QODE.conf"));
#endif
}

void AppPaths::initQtSettings()
{
#ifdef Q_OS_UNIX
    // Every QSettings in QODE names its file itself (SettingsManager), so only Qt's QSettings(scope, organization)
    // objects follow this.
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, config());
#endif
    // Windows: QSettings(scope, organization) is always the registry there and setPath cannot move it, so the file
    // dialogs go through FileDialogs, which puts HKCU\Software\QtProject back the way it was after each dialog.
}
