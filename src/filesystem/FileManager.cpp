#include "FileManager.h"
#include "platform/Platform.h"

#include "history/LocalHistory.h"

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
    // On Windows a rename that only changes case (Foo.kt -> foo.kt) finds the file itself at `to`: no clash.
    if (QFileInfo::exists(to) && !(from != to && Platform::samePath(from, to))) {
        if (error)
            *error = QStringLiteral("\"%1\" already exists.").arg(QFileInfo(to).fileName());
        return false;
    }
    if (!QFile::rename(from, to)) {
        if (error)
            *error = QStringLiteral("Could not rename \"%1\".").arg(QFileInfo(from).fileName());
        return false;
    }
    LocalHistory::pathMoved(from, to);
    return true;
}

QString uniquePath(const QString &dir, const QString &name)
{
    const QDir d(dir);
    if (!QFileInfo::exists(d.filePath(name)) && !QFileInfo(d.filePath(name)).isSymLink())
        return d.filePath(name);
    const QFileInfo fi(name);
    QString base = fi.completeBaseName(), suffix = fi.suffix();
    if (base.isEmpty()) { // dot files: ".env" has no base name
        base = name;
        suffix.clear();
    }
    const QString ext = suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix;
    for (int n = 1;; ++n) {
        const QString cand = base + QStringLiteral(" copy") + (n > 1 ? QLatin1Char(' ') + QString::number(n) : QString()) + ext;
        if (!QFileInfo::exists(d.filePath(cand)) && !QFileInfo(d.filePath(cand)).isSymLink())
            return d.filePath(cand);
    }
}

static bool copyTree(const QString &from, const QString &to, QString *error)
{
    const QFileInfo fi(from);
    if (fi.isSymLink()) {
        if (!QFile::link(fi.symLinkTarget(), to)) {
            if (error)
                *error = QStringLiteral("Could not copy \"%1\".").arg(fi.fileName());
            return false;
        }
        return true;
    }
    if (fi.isDir()) {
        if (!QDir().mkpath(to)) {
            if (error)
                *error = QStringLiteral("Could not create \"%1\".").arg(QFileInfo(to).fileName());
            return false;
        }
        const auto entries = QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &e : entries)
            if (!copyTree(e.absoluteFilePath(), QDir(to).filePath(e.fileName()), error))
                return false;
        return true;
    }
    if (!QFile::copy(from, to)) {
        if (error)
            *error = QStringLiteral("Could not copy \"%1\".").arg(fi.fileName());
        return false;
    }
    return true;
}

QString copyInto(const QString &source, const QString &targetDir, QString *error)
{
    const QFileInfo fi(source);
    if (!fi.exists() && !fi.isSymLink()) {
        if (error)
            *error = QStringLiteral("\"%1\" no longer exists.").arg(fi.fileName());
        return {};
    }
    const QString src = fi.absoluteFilePath();
    if (fi.isDir() && !fi.isSymLink() && (targetDir == src || targetDir.startsWith(src + QLatin1Char('/')))) {
        if (error)
            *error = QStringLiteral("Can't copy \"%1\" into itself.").arg(fi.fileName());
        return {};
    }
    const QString dest = uniquePath(targetDir, fi.fileName());
    if (!copyTree(src, dest, error)) {
        removePath(dest, nullptr); // no half-copied leftovers
        return {};
    }
    return dest;
}

bool isScript(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isFile())
        return false;
    static const QStringList kShell{QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("ksh")};
    if (kShell.contains(fi.suffix().toLower()))
        return true;
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(2) == "#!";
}

bool makeExecutable(const QString &path, QString *error)
{
    QFile f(path);
    QFileDevice::Permissions p = f.permissions();
    if (p & QFileDevice::ReadOwner)
        p |= QFileDevice::ExeOwner | QFileDevice::ExeUser;
    if (p & QFileDevice::ReadGroup)
        p |= QFileDevice::ExeGroup;
    if (p & QFileDevice::ReadOther)
        p |= QFileDevice::ExeOther;
    if (f.setPermissions(p))
        return true;
    if (error)
        *error = f.errorString();
    return false;
}

bool removePath(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    if (!fi.isSymLink())
        LocalHistory::recordDisk(path, LocalHistory::Kind::BeforeDelete); // the text files stay restorable
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
