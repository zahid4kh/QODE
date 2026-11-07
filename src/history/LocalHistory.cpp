#include "LocalHistory.h"
#include "platform/Platform.h"
#include "platform/AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace LocalHistory {
namespace {

constexpr qint64 kMaxBytes = 2 * 1024 * 1024;
constexpr int kMaxVersions = 100;
constexpr qint64 kMaxAgeMs = 30LL * 24 * 3600 * 1000;
constexpr int kMaxFolderFiles = 3000;

Kind g_reason = Kind::Saved;

const char *kindToken(Kind k)
{
    switch (k) {
    case Kind::Saved: return "save";
    case Kind::AutoSaved: return "auto";
    case Kind::QodeEdit: return "qode";
    case Kind::Refactor: return "refactor";
    case Kind::BeforeDelete: return "delete";
    case Kind::BeforeRestore: return "restore";
    case Kind::OnDisk: return "disk";
    }
    return "save";
}

Kind kindFromToken(const QString &t)
{
    for (Kind k : {Kind::Saved, Kind::AutoSaved, Kind::QodeEdit, Kind::Refactor, Kind::BeforeDelete, Kind::BeforeRestore, Kind::OnDisk})
        if (t == QLatin1String(kindToken(k)))
            return k;
    return Kind::Saved;
}

QString folderFor(const QString &path)
{
    const QByteArray h = QCryptographicHash::hash(Platform::pathKey(QDir::cleanPath(path)).toUtf8(), QCryptographicHash::Sha1).toHex().left(20);
    return storageRoot() + QLatin1Char('/') + QString::fromLatin1(h);
}

QString contentHash(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(12));
}

bool storable(const QByteArray &data)
{
    return data.size() <= kMaxBytes && !data.left(8000).contains('\0');
}

// The stored versions of a folder, oldest first (the names sort by time).
QStringList versionFiles(const QString &folder)
{
    return QDir(folder).entryList({QStringLiteral("*.z")}, QDir::Files, QDir::Name);
}

bool isSkippedFolder(const QString &name)
{
    static const QSet<QString> skip{QStringLiteral(".git"), QStringLiteral("node_modules"), QStringLiteral("build"), QStringLiteral("dist"),
                                    QStringLiteral("target"), QStringLiteral(".gradle"), QStringLiteral("venv"), QStringLiteral(".venv"),
                                    QStringLiteral("__pycache__"), QStringLiteral(".idea"), QStringLiteral(".next"), QStringLiteral("out")};
    return skip.contains(name);
}

} // namespace

QString storageRoot()
{
    return AppPaths::data(QStringLiteral("history"));
}

QString label(Kind kind)
{
    switch (kind) {
    case Kind::Saved: return QCoreApplication::translate("LocalHistory", "Saved");
    case Kind::AutoSaved: return QCoreApplication::translate("LocalHistory", "Auto-saved");
    case Kind::QodeEdit: return QCoreApplication::translate("LocalHistory", "QODE edit");
    case Kind::Refactor: return QCoreApplication::translate("LocalHistory", "Refactoring");
    case Kind::BeforeDelete: return QCoreApplication::translate("LocalHistory", "Before delete");
    case Kind::BeforeRestore: return QCoreApplication::translate("LocalHistory", "Before restore");
    case Kind::OnDisk: return QCoreApplication::translate("LocalHistory", "On disk");
    }
    return {};
}

Reason::Reason(Kind kind)
    : m_previous(g_reason)
{
    g_reason = kind;
}

Reason::~Reason()
{
    g_reason = m_previous;
}

Kind currentReason()
{
    return g_reason;
}

void record(const QString &path, const QByteArray &content, Kind kind)
{
    if (path.isEmpty() || !storable(content) || QDir::cleanPath(path).startsWith(storageRoot()))
        return;
    const QString folder = folderFor(path);
    const QString hash = contentHash(content);
    QStringList files = versionFiles(folder);
    if (!files.isEmpty() && files.last().endsWith(QLatin1Char('-') + hash + QStringLiteral(".z")))
        return; // the newest version already is this text
    if (!QDir().mkpath(folder))
        return;
    QFile::setPermissions(storageRoot(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString pathFile = folder + QStringLiteral("/path");
    if (!QFileInfo::exists(pathFile)) {
        QSaveFile p(pathFile);
        if (p.open(QIODevice::WriteOnly)) {
            p.write(QDir::cleanPath(path).toUtf8());
            p.commit();
        }
    }
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (kind == Kind::AutoSaved && !files.isEmpty() && files.last().section(QLatin1Char('-'), 1, 1) == QLatin1String(kindToken(kind)) &&
        files.last().section(QLatin1Char('-'), 0, 0).toLongLong() / 60000 == now / 60000)
        QFile::remove(folder + QLatin1Char('/') + files.takeLast()); // auto save while typing: one version a minute
    if (!files.isEmpty()) // two versions in the same millisecond still sort in the order they came
        now = qMax(now, files.last().section(QLatin1Char('-'), 0, 0).toLongLong() + 1);
    const QString name = QStringLiteral("%1-%2-%3.z").arg(now, 13, 10, QLatin1Char('0')).arg(QLatin1String(kindToken(kind)), hash);
    QSaveFile f(folder + QLatin1Char('/') + name);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(qCompress(content, 6));
    if (!f.commit())
        return;
    files.append(name);
    while (files.size() > kMaxVersions)
        QFile::remove(folder + QLatin1Char('/') + files.takeFirst());
}

void recordDisk(const QString &path, Kind kind)
{
    const QFileInfo fi(path);
    if (fi.isFile()) {
        if (fi.size() > kMaxBytes)
            return;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            record(fi.absoluteFilePath(), f.readAll(), kind);
        return;
    }
    if (!fi.isDir())
        return;
    int count = 0;
    QStringList stack{fi.absoluteFilePath()};
    while (!stack.isEmpty() && count < kMaxFolderFiles) {
        const QDir dir(stack.takeLast());
        for (const QFileInfo &e : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
            if (e.isDir()) {
                if (!isSkippedFolder(e.fileName()))
                    stack.append(e.absoluteFilePath());
            } else if (e.isFile() && e.size() <= kMaxBytes && ++count <= kMaxFolderFiles) {
                QFile f(e.absoluteFilePath());
                if (f.open(QIODevice::ReadOnly))
                    record(e.absoluteFilePath(), f.readAll(), kind);
            }
        }
    }
}

void recordWrite(const QString &path, const QByteArray &written, Kind kind)
{
    const QFileInfo fi(path);
    if (fi.isFile() && fi.size() <= kMaxBytes) {
        // The version on disk before this write: stored only when no copy has it yet (the first save of a file, or one
        // that changed outside QODE).
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray old = f.readAll();
            const QStringList files = versionFiles(folderFor(path));
            if (old != written && (files.isEmpty() || !files.last().endsWith(QLatin1Char('-') + contentHash(old) + QStringLiteral(".z"))))
                record(path, old, Kind::OnDisk);
        }
    }
    record(path, written, kind);
}

QVector<Version> versions(const QString &path)
{
    QVector<Version> out;
    const QString folder = folderFor(path);
    const QStringList files = versionFiles(folder);
    for (auto it = files.crbegin(); it != files.crend(); ++it) {
        const QStringList parts = it->chopped(2).split(QLatin1Char('-'));
        if (parts.size() < 3)
            continue;
        Version v;
        v.file = folder + QLatin1Char('/') + *it;
        v.time = parts.at(0).toLongLong();
        v.kind = kindFromToken(parts.at(1));
        out.append(v);
    }
    return out;
}

QByteArray content(const Version &version)
{
    QFile f(version.file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return qUncompress(f.readAll());
}

QVector<TrackedFile> filesUnder(const QString &folder)
{
    QVector<TrackedFile> out;
    const QString prefix = QDir::cleanPath(folder) + QLatin1Char('/');
    const QDir root(storageRoot());
    for (const QString &entry : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile p(root.filePath(entry) + QStringLiteral("/path"));
        if (!p.open(QIODevice::ReadOnly))
            continue;
        const QString path = QString::fromUtf8(p.readAll());
        if (!Platform::isUnder(path, prefix.chopped(1)))
            continue;
        const QStringList files = versionFiles(root.filePath(entry));
        if (files.isEmpty())
            continue;
        TrackedFile t;
        t.path = path;
        t.versions = int(files.size());
        t.newest = files.last().section(QLatin1Char('-'), 0, 0).toLongLong();
        t.exists = QFileInfo(path).isFile();
        out.append(t);
    }
    std::sort(out.begin(), out.end(), [](const TrackedFile &a, const TrackedFile &b) { return a.newest > b.newest; });
    return out;
}

void pathMoved(const QString &from, const QString &to)
{
    const QString src = QDir::cleanPath(from), dst = QDir::cleanPath(to);
    if (src == dst)
        return;
    QVector<QPair<QString, QString>> moves; // old file path -> new file path
    const QDir root(storageRoot());
    for (const QString &entry : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile p(root.filePath(entry) + QStringLiteral("/path"));
        if (!p.open(QIODevice::ReadOnly))
            continue;
        const QString path = QString::fromUtf8(p.readAll());
        if (Platform::samePath(path, src))
            moves.append({path, dst});
        else if (Platform::isUnder(path, src))
            moves.append({path, dst + path.mid(src.size())});
    }
    for (const auto &m : std::as_const(moves)) {
        const QString oldFolder = folderFor(m.first), newFolder = folderFor(m.second);
        const bool sameFolder = oldFolder == newFolder; // Windows: a rename that only changes case keeps its folder
        QDir().mkpath(newFolder);
        // Versions already stored under the new path (a file that was there before) stay; the names never collide.
        if (!sameFolder)
            for (const QString &f : versionFiles(oldFolder))
                QFile::rename(oldFolder + QLatin1Char('/') + f, newFolder + QLatin1Char('/') + f);
        QSaveFile p(newFolder + QStringLiteral("/path"));
        if (p.open(QIODevice::WriteOnly)) {
            p.write(m.second.toUtf8());
            p.commit();
        }
        if (!sameFolder)
            QDir(oldFolder).removeRecursively();
    }
}

void prune()
{
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - kMaxAgeMs;
    const QDir root(storageRoot());
    for (const QString &entry : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString folder = root.filePath(entry);
        QStringList files = versionFiles(folder);
        while (!files.isEmpty() && files.first().section(QLatin1Char('-'), 0, 0).toLongLong() < cutoff)
            QFile::remove(folder + QLatin1Char('/') + files.takeFirst());
        if (files.isEmpty())
            QDir(folder).removeRecursively();
    }
}

} // namespace LocalHistory
