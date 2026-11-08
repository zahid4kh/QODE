#include "editor/TextEncoding.h"
#include "platform/Platform.h"

#include <QStringConverter>

namespace {

const QString kUtf8 = QStringLiteral("UTF-8");
const QString kUtf8Bom = QStringLiteral("UTF-8 BOM");
const QString kUtf16Le = QStringLiteral("UTF-16 LE");
const QString kUtf16Be = QStringLiteral("UTF-16 BE");
const QString kLatin1 = QStringLiteral("Latin-1");
const QString kCp1252 = QStringLiteral("Windows-1252");

// Windows-1252 0x80-0x9F. The five bytes it leaves undefined keep their Latin-1 meaning (U+0081 …), so any byte
// still reads and writes back unchanged.
const char16_t kCp1252High[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

QString fromCp1252(const QByteArray &data)
{
    QString out(data.size(), Qt::Uninitialized);
    QChar *o = out.data();
    for (const char ch : data) {
        const auto b = uchar(ch);
        *o++ = QChar((b >= 0x80 && b < 0xA0) ? kCp1252High[b - 0x80] : char16_t(b));
    }
    return out;
}

// False when `text` holds a character Windows-1252 has no byte for.
bool toCp1252(const QString &text, QByteArray *out)
{
    out->resize(text.size());
    char *o = out->data();
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) {
            *o++ = char(u);
            continue;
        }
        int found = -1;
        for (int i = 0; i < 32; ++i)
            if (kCp1252High[i] == u) {
                found = i;
                break;
            }
        if (found < 0)
            return false;
        *o++ = char(0x80 + found);
    }
    return true;
}

QString legacyDecode(const QString &encoding, const QByteArray &data)
{
    return encoding == kCp1252 ? fromCp1252(data) : QString::fromLatin1(data);
}

} // namespace

namespace TextEncoding {

QStringList names()
{
    return {kUtf8, kUtf8Bom, kUtf16Le, kUtf16Be, kLatin1, kCp1252};
}

Decoded decode(const QByteArray &bytes, const QString &forced)
{
    Decoded d;
    QByteArray data = bytes;
    QString enc = forced;
    if (enc.isEmpty()) {
        if (data.startsWith("\xFF\xFE"))
            enc = kUtf16Le;
        else if (data.startsWith("\xFE\xFF"))
            enc = kUtf16Be;
        else if (data.startsWith("\xEF\xBB\xBF"))
            enc = kUtf8Bom;
    }
    // The BOM is not text.
    if ((enc == kUtf16Le && data.startsWith("\xFF\xFE")) || (enc == kUtf16Be && data.startsWith("\xFE\xFF")))
        data.remove(0, 2);
    else if ((enc == kUtf8Bom || enc == kUtf8) && data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);

    if (enc == kUtf16Le || enc == kUtf16Be) {
        QStringDecoder dec(enc == kUtf16Le ? QStringConverter::Utf16LE : QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
        d.text = dec.decode(data);
        d.encoding = enc;
        return d;
    }
    if (enc == kLatin1 || enc == kCp1252) {
        d.text = legacyDecode(enc, data);
        d.encoding = enc;
        return d;
    }
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString text = utf8.decode(data);
    // A UTF-8 the user chose shows bad bytes as U+FFFD; a detected one (a BOM, then bytes that are not UTF-8) falls back.
    if (!utf8.hasError() || (!forced.isEmpty() && (enc == kUtf8 || enc == kUtf8Bom))) {
        d.text = text;
        d.encoding = enc.isEmpty() ? kUtf8 : enc;
        return d;
    }
    // Not UTF-8: the platform's legacy encoding, which maps every byte.
    d.encoding = Platform::legacyEncoding();
    d.text = legacyDecode(d.encoding, data);
    return d;
}

QByteArray encode(const QString &text, QString *encoding)
{
    const QString enc = encoding ? *encoding : kUtf8;
    if (enc == kUtf16Le || enc == kUtf16Be) {
        QStringEncoder e(enc == kUtf16Le ? QStringConverter::Utf16LE : QStringConverter::Utf16BE);
        const QByteArray body = e.encode(text);
        return (enc == kUtf16Le ? QByteArray("\xFF\xFE", 2) : QByteArray("\xFE\xFF", 2)) + body;
    }
    if (enc == kLatin1 || enc == kCp1252) {
        QByteArray out;
        const bool fits = enc == kCp1252 ? toCp1252(text, &out) : (out = text.toLatin1(), QString::fromLatin1(out) == text);
        if (fits)
            return out;
        // Typed after loading, a character the legacy encoding can't hold: the file becomes UTF-8.
        if (encoding)
            *encoding = kUtf8;
        return text.toUtf8();
    }
    QByteArray out = text.toUtf8();
    if (enc == kUtf8Bom)
        out.prepend("\xEF\xBB\xBF");
    return out;
}

} // namespace TextEncoding
