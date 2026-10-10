#include "JarSource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QVector>
#include <QStandardPaths>
#include <QUrl>

#include <zlib.h>

namespace {

QHash<QString, QString> &unpacked() // cached file -> jar URI
{
    static QHash<QString, QString> map;
    return map;
}

quint16 u16(const QByteArray &b, int off) { return quint16(quint8(b[off])) | quint16(quint8(b[off + 1])) << 8; }
quint32 u32(const QByteArray &b, int off) { return quint32(u16(b, off)) | quint32(u16(b, off + 2)) << 16; }

// "jar:///a/b.jar!/p/X.kt" or "jar:file:///a/b.jar!/p/X.kt" -> jar file, entry name
bool split(const QString &uri, QString *jar, QString *entry)
{
    QString rest = uri.mid(4); // after "jar:"
    if (rest.startsWith(QLatin1String("file:")))
        rest.remove(0, 5);
    if (rest.startsWith(QLatin1String("//")))
        rest.remove(0, 2);
    const int bang = rest.indexOf(QLatin1String("!/"));
    if (bang < 0)
        return false;
    *jar = QUrl::fromPercentEncoding(rest.left(bang).toUtf8());
    *entry = QUrl::fromPercentEncoding(rest.mid(bang + 2).toUtf8());
    return !jar->isEmpty() && !entry->isEmpty();
}

// Reads one entry out of a zip/jar (stored or deflated; no zip64, which source jars never need).
bool readEntry(const QString &jarPath, const QString &entry, QByteArray *out, QString *error)
{
    QFile f(jarPath);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot open %1").arg(jarPath);
        return false;
    }
    const qint64 size = f.size();
    const qint64 tail = qMin<qint64>(size, 22 + 65535);
    f.seek(size - tail);
    const QByteArray end = f.read(tail);
    int eocd = -1;
    for (int i = int(end.size()) - 22; i >= 0; --i)
        if (u32(end, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        *error = QStringLiteral("Not a zip file: %1").arg(jarPath);
        return false;
    }
    const quint32 cdSize = u32(end, eocd + 12), cdOffset = u32(end, eocd + 16);
    if (cdOffset == 0xFFFFFFFFu || cdSize == 0xFFFFFFFFu) {
        *error = QStringLiteral("zip64 archives are not supported");
        return false;
    }
    f.seek(cdOffset);
    const QByteArray cd = f.read(cdSize);
    const QByteArray wanted = entry.toUtf8();
    int p = 0;
    while (p + 46 <= cd.size() && u32(cd, p) == 0x02014b50) {
        const quint16 method = u16(cd, p + 10);
        const quint32 compSize = u32(cd, p + 20), rawSize = u32(cd, p + 24);
        const quint16 nameLen = u16(cd, p + 28), extraLen = u16(cd, p + 30), commentLen = u16(cd, p + 32);
        const quint32 local = u32(cd, p + 42);
        if (QByteArray::fromRawData(cd.constData() + p + 46, nameLen) == wanted) {
            if (rawSize > 64u * 1024 * 1024) {
                *error = QStringLiteral("Entry is too large");
                return false;
            }
            f.seek(local);
            const QByteArray lh = f.read(30);
            if (lh.size() < 30 || u32(lh, 0) != 0x04034b50) {
                *error = QStringLiteral("Corrupt zip entry");
                return false;
            }
            f.seek(qint64(local) + 30 + u16(lh, 26) + u16(lh, 28));
            const QByteArray data = f.read(compSize);
            if (method == 0) {
                *out = data;
                return true;
            }
            if (method != 8) {
                *error = QStringLiteral("Unsupported compression method");
                return false;
            }
            out->resize(int(rawSize));
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
                *error = QStringLiteral("zlib failure");
                return false;
            }
            z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
            z.avail_in = uInt(data.size());
            z.next_out = reinterpret_cast<Bytef *>(out->data());
            z.avail_out = uInt(out->size());
            const int rc = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (rc != Z_STREAM_END) {
                *error = QStringLiteral("Corrupt zip entry");
                return false;
            }
            return true;
        }
        p += 46 + nameLen + extraLen + commentLen;
    }
    *error = QStringLiteral("%1 is not in %2").arg(entry, QFileInfo(jarPath).fileName());
    return false;
}

// Value of the SourceFile attribute of a class file (the .kt / .java it was compiled from), or "".
QString sourceFileOf(const QByteArray &c)
{
    if (c.size() < 10 || quint8(c[0]) != 0xCA || quint8(c[1]) != 0xFE || quint8(c[2]) != 0xBA || quint8(c[3]) != 0xBE)
        return {};
    auto be16 = [&](int o) { return o + 2 <= c.size() ? (quint16(quint8(c[o])) << 8 | quint8(c[o + 1])) : 0; };
    auto be32 = [&](int o) { return o + 4 <= c.size() ? (quint32(be16(o)) << 16 | be16(o + 2)) : 0u; };
    const int count = be16(8);
    QVector<QPair<int, int>> utf(count + 1, {-1, 0}); // offset, length of each Utf8 constant
    int p = 10;
    for (int i = 1; i < count && p < c.size(); ++i) {
        switch (quint8(c[p])) {
        case 1: utf[i] = {p + 3, be16(p + 1)}; p += 3 + be16(p + 1); break;
        case 3: case 4: case 9: case 10: case 11: case 12: case 17: case 18: p += 5; break;
        case 5: case 6: p += 9; ++i; break;
        case 15: p += 4; break;
        default: p += 3; break; // 7 8 16 19 20
        }
    }
    p += 6; // access, this, super
    p += 2 + 2 * be16(p); // interfaces
    for (int pass = 0; pass < 2; ++pass) { // fields, methods
        int n = be16(p);
        p += 2;
        while (n-- > 0 && p < c.size()) {
            int attrs = be16(p + 6);
            p += 8;
            while (attrs-- > 0 && p < c.size())
                p += 6 + int(be32(p + 2));
        }
    }
    int attrs = be16(p);
    p += 2;
    while (attrs-- > 0 && p + 6 <= c.size()) {
        const int name = be16(p);
        if (name > 0 && name <= count && utf[name].first >= 0 && c.mid(utf[name].first, utf[name].second) == "SourceFile") {
            const int v = be16(p + 6);
            if (v > 0 && v <= count && utf[v].first >= 0)
                return QString::fromUtf8(c.mid(utf[v].first, utf[v].second));
            return {};
        }
        p += 6 + int(be32(p + 2));
    }
    return {};
}

QString g_failure, g_missingJar;
bool g_class = false;

// The sources jar belonging to `jar`: next to it (Maven, and Gradle's own copies), or in a sibling folder of the
// version directory (Gradle's cache keeps every file in a folder named by its hash).
QString findSourcesJar(const QString &jar)
{
    const QFileInfo fi(jar);
    const QString name = fi.completeBaseName() + QStringLiteral("-sources.jar");
    if (QFile::exists(fi.absolutePath() + QLatin1Char('/') + name))
        return fi.absolutePath() + QLatin1Char('/') + name;
    QDir version(fi.absolutePath());
    if (!version.cdUp())
        return {};
    const QStringList hashes = version.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &h : hashes)
        if (QFile::exists(version.filePath(h + QLatin1Char('/') + name)))
            return version.filePath(h + QLatin1Char('/') + name);
    return {};
}

} // namespace

namespace JarSource {

QString lastFailure() { return g_failure; }
QString lastMissingSourcesJar() { return g_missingJar; }
bool lastWasClass() { return g_class; }

Coordinates coordinatesOf(const QString &jarPath)
{
    // ~/.gradle/caches/modules-2/files-2.1/<group>/<artifact>/<version>/<hash>/<file>.jar
    static const QString marker = QStringLiteral("/files-2.1/");
    const int i = jarPath.indexOf(marker);
    if (i >= 0) {
        const QStringList parts = jarPath.mid(i + marker.size()).split(QLatin1Char('/'));
        if (parts.size() >= 5)
            return {parts[0], parts[1], parts[2]};
    }
    // ~/.m2/repository/<group as folders>/<artifact>/<version>/<file>.jar
    static const QString m2 = QStringLiteral("/.m2/repository/");
    const int j = jarPath.indexOf(m2);
    if (j >= 0) {
        QStringList parts = jarPath.mid(j + m2.size()).split(QLatin1Char('/'));
        if (parts.size() >= 4) {
            parts.removeLast();
            const QString version = parts.takeLast(), artifact = parts.takeLast();
            return {parts.join(QLatin1Char('.')), artifact, version};
        }
    }
    return {};
}

bool isJarUri(const QString &uri) { return uri.startsWith(QLatin1String("jar:")); }

QString extract(const QString &uri, QString *error)
{
    QString err;
    QString *e = error ? error : &err;
    QString jar, entry;
    g_failure.clear();
    g_missingJar.clear();
    g_class = false;
    if (!split(uri, &jar, &entry)) {
        *e = QStringLiteral("Unsupported location: %1").arg(uri);
        return {};
    }
    if (entry.endsWith(QLatin1String(".class"))) {
        // Compiled code is unreadable: open the source file it was built from, from the library's sources jar.
        const QString cls = QFileInfo(entry).completeBaseName();
        const QString sources = findSourcesJar(jar);
        if (sources.isEmpty()) {
            g_failure = QStringLiteral("No sources available for %1").arg(cls);
            g_missingJar = jar;
            *e = g_failure;
            return {};
        }
        QByteArray bytes;
        QString ignored;
        readEntry(jar, entry, &bytes, &ignored);
        const QString dir = entry.contains(QLatin1Char('/')) ? entry.left(entry.lastIndexOf(QLatin1Char('/')) + 1) : QString();
        QString outer = cls.left(cls.indexOf(QLatin1Char('$')) < 0 ? cls.size() : cls.indexOf(QLatin1Char('$')));
        QStringList names;
        const QString declared = sourceFileOf(bytes);
        if (!declared.isEmpty())
            names << declared;
        if (outer.endsWith(QLatin1String("Kt")))
            names << outer.chopped(2) + QStringLiteral(".kt");
        names << outer + QStringLiteral(".kt") << outer + QStringLiteral(".java");
        for (const QString &n : names) {
            QByteArray probe;
            if (readEntry(sources, dir + n, &probe, &ignored)) {
                const QString path = extract(QStringLiteral("jar://") + QString::fromLatin1(QUrl::toPercentEncoding(sources, "/")) + QStringLiteral("!/") +
                                                 QString::fromLatin1(QUrl::toPercentEncoding(dir + n, "/")),
                                             error);
                g_class = !path.isEmpty();
                return path;
            }
        }
        g_failure = QStringLiteral("%1 is not in the sources jar of this library").arg(cls);
        *e = g_failure;
        return {};
    }
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(jar.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE/lsp/library-sources/") +
                        hash + QLatin1Char('-') + QFileInfo(jar).completeBaseName();
    const QString target = QDir::cleanPath(dir + QLatin1Char('/') + entry);
    if (!target.startsWith(dir + QLatin1Char('/'))) { // "../" in a hostile entry name
        *e = QStringLiteral("Unsafe entry name");
        return {};
    }
    QByteArray data;
    if (!readEntry(jar, entry, &data, e))
        return {};
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target); // earlier copies are read-only
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
        *e = QStringLiteral("Cannot write %1").arg(target);
        return {};
    }
    out.close();
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    unpacked().insert(target, uri);
    return target;
}

QString storeText(const QString &uri, const QByteArray &text)
{
    // jdt://contents/rt.jar/java.lang/String.class?=project/... : the class file's name decides the file name.
    const QString name = QFileInfo(QUrl(uri).path()).completeBaseName();
    if (name.isEmpty())
        return {};
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(uri.toUtf8(), QCryptographicHash::Md5).toHex().left(12));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE/lsp/library-sources/jdt/") + hash;
    const QString target = dir + QLatin1Char('/') + name + QStringLiteral(".java");
    QDir().mkpath(dir);
    QFile::remove(target); // earlier copies are read-only
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size())
        return {};
    out.close();
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    unpacked().insert(target, uri);
    return target;
}

QString uriForPath(const QString &path) { return unpacked().value(path); }

bool isLibraryPath(const QString &path)
{
    static const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE/lsp/library-sources/");
    return path.startsWith(base);
}

} // namespace JarSource
