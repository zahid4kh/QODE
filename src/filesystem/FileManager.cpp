#include "FileManager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUrl>

namespace FileManager {

ReadResult readFile(const QString &path)
{
    ReadResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = f.errorString();
        return r;
    }
    r.data = f.readAll();
    if (f.error() != QFile::NoError) {
        r.error = f.errorString();
        return r;
    }
    r.ok = true;
    return r;
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    // QSaveFile writes to a temp file and renames, so a failed save never truncates the original.
    QSaveFile f(path);
    f.setDirectWriteFallback(true);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool createFile(const QString &path, QString *error)
{
    if (QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("\"%1\" already exists.").arg(QFileInfo(path).fileName());
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool createFolder(const QString &path, QString *error)
{
    if (QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("\"%1\" already exists.").arg(QFileInfo(path).fileName());
        return false;
    }
    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Could not create the directory.");
        return false;
    }
    return true;
}

bool renamePath(const QString &from, const QString &to, QString *error)
{
    if (QFileInfo::exists(to)) {
        if (error)
            *error = QStringLiteral("\"%1\" already exists.").arg(QFileInfo(to).fileName());
        return false;
    }
    if (!QFile::rename(from, to)) {
        if (error)
            *error = QStringLiteral("Could not rename \"%1\".").arg(QFileInfo(from).fileName());
        return false;
    }
    return true;
}

bool removePath(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    bool ok;
    if (fi.isDir() && !fi.isSymLink())
        ok = QDir(path).removeRecursively();
    else
        ok = QFile::remove(path);
    if (!ok && error)
        *error = QStringLiteral("Could not delete \"%1\" (permission denied or in use).").arg(fi.fileName());
    return ok;
}

QString validateName(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty())
        return QStringLiteral("Name cannot be empty.");
    if (n == QLatin1String(".") || n == QLatin1String(".."))
        return QStringLiteral("Invalid name.");
    if (n.contains(QLatin1Char('/')) || n.contains(QChar(0)))
        return QStringLiteral("Name cannot contain '/'.");
    return {};
}

void revealInFileManager(const QString &path)
{
    const QFileInfo fi(path);
    const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

QString displayPath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

} // namespace FileManager
