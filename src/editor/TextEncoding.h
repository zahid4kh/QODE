#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// How a text file's bytes become text and back. The encodings, by the name the status bar shows:
// - "UTF-8", "UTF-8 BOM"
// - "UTF-16 LE", "UTF-16 BE" (always with their BOM: what Windows tools write, PowerShell 5's > for one)
// - "Latin-1" (ISO 8859-1)
// - "Windows-1252" (Windows' "ANSI" code page: Latin-1 except 0x80-0x9F, which hold € ‚ „ … “ ” – — and others)
//
// Detection: a BOM decides; else strict UTF-8; else the platform's legacy encoding (Platform::legacyEncoding:
// Latin-1 on Unix, Windows-1252 on Windows). Both legacy encodings give every byte a character, so saving writes the
// original bytes back.
namespace TextEncoding {

QStringList names();

struct Decoded {
    QString text;
    QString encoding;
};

// `forced` (one of names()) skips detection, for "Reopen with Encoding"; a BOM that matches it is still dropped.
Decoded decode(const QByteArray &data, const QString &forced = {});

// The bytes for `text` in `encoding`, BOM included where the encoding has one. A character a legacy encoding cannot
// hold makes it UTF-8 instead: *encoding is changed then (when given).
QByteArray encode(const QString &text, QString *encoding);

} // namespace TextEncoding
