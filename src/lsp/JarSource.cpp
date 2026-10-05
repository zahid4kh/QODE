#include "JarSource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

} // namespace

namespace JarSource {

bool isJarUri(const QString &uri) { return uri.startsWith(QLatin1String("jar:")); }

QString extract(const QString &uri, QString *error)
{
    QString err;
    QString *e = error ? error : &err;
    QString jar, entry;
    if (!split(uri, &jar, &entry)) {
        *e = QStringLiteral("Unsupported location: %1").arg(uri);
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
