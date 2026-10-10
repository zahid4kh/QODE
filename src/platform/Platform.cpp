#include "Platform.h"

#include <QDir>
#include <QStandardPaths>

namespace Platform {

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/QODE");
}

QString dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/QODE");
}

QString cacheDir()
{
#ifdef Q_OS_WIN
    return dataDir() + QStringLiteral("/cache");
#else
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE");
#endif
}

QString runDir()
{
    return cacheDir() + QStringLiteral("/run");
}

QString userBinDir()
{
    return QDir::homePath() + QStringLiteral("/.local/bin");
}

} // namespace Platform
