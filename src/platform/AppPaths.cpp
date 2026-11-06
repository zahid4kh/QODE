#include "platform/AppPaths.h"

#include <QDir>
#include <QStandardPaths>

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

} // namespace

QString AppPaths::config(const QString &sub)
{
#ifdef Q_OS_WIN
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
