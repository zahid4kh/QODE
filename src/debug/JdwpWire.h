#pragma once

// Encoding helpers for JDWP packets (https://docs.oracle.com/en/java/javase/21/docs/specs/jdwp/jdwp-protocol.html):
// everything is big-endian; strings are a 4-byte length + UTF-8.

#include <QByteArray>
#include <QString>

namespace Jdwp {

struct Reader
{
    const QByteArray &d;
    int pos = 0;
    bool bad = false;

    quint64 num(int size)
    {
        if (size < 0 || pos + size > d.size()) {
            bad = true;
            return 0;
        }
        quint64 v = 0;
        for (int i = 0; i < size; ++i)
            v = (v << 8) | quint8(d.at(pos + i));
        pos += size;
        return v;
    }
    qint32 i32() { return qint32(num(4)); }
    quint8 u8() { return quint8(num(1)); }
    QString str()
    {
        const qint32 n = i32();
        if (bad || n < 0 || pos + n > d.size()) {
            bad = true;
            return {};
        }
        const QString s = QString::fromUtf8(d.constData() + pos, n);
        pos += n;
        return s;
    }
    bool atEnd() const { return pos >= d.size(); }
};

inline void put(QByteArray &out, quint64 v, int size)
{
    for (int i = size - 1; i >= 0; --i)
        out.append(char((v >> (8 * i)) & 0xff));
}

inline void putString(QByteArray &out, const QString &s)
{
    const QByteArray u = s.toUtf8();
    put(out, quint64(u.size()), 4);
    out.append(u);
}

// Bytes of an untagged value of this tag (object ids for every reference tag).
inline int valueSize(char tag, int objectSize)
{
    switch (tag) {
    case 'B': case 'Z': return 1;
    case 'C': case 'S': return 2;
    case 'F': case 'I': return 4;
    case 'D': case 'J': return 8;
    case 'V': return 0;
    default: return objectSize; // L s t g l c [
    }
}

inline bool isReferenceTag(char tag)
{
    return QByteArrayView("Lstglc[").contains(tag);
}

// "Lcom/example/Main$Inner;" -> "com.example.Main$Inner"; "[I" -> "int[]".
inline QString typeName(const QString &signature)
{
    if (signature.startsWith(QLatin1Char('[')))
        return typeName(signature.mid(1)) + QStringLiteral("[]");
    if (signature.size() == 1) {
        switch (signature.at(0).toLatin1()) {
        case 'Z': return QStringLiteral("boolean");
        case 'B': return QStringLiteral("byte");
        case 'C': return QStringLiteral("char");
        case 'S': return QStringLiteral("short");
        case 'I': return QStringLiteral("int");
        case 'J': return QStringLiteral("long");
        case 'F': return QStringLiteral("float");
        case 'D': return QStringLiteral("double");
        case 'V': return QStringLiteral("void");
        default: break;
        }
    }
    QString s = signature;
    if (s.startsWith(QLatin1Char('L')) && s.endsWith(QLatin1Char(';')))
        s = s.mid(1, s.size() - 2);
    s.replace(QLatin1Char('/'), QLatin1Char('.'));
    return s;
}

} // namespace Jdwp
