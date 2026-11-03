#pragma once

#include <QString>
#include <QStringList>

// .editorconfig support (https://spec.editorconfig.org): the properties that apply to one file, read from every
// .editorconfig between the file and the first one marked `root = true`. Nearer files and later sections win.
// Parsed files are cached by path and modification time.
namespace EditorConfig {

struct Properties {
    int useTabs = -1;                // indent_style: -1 unset, 0 space, 1 tab
    int indentSize = 0;              // indent_size (`tab` resolves to tab_width); 0 unset
    int tabWidth = 0;                // tab_width (defaults to a numeric indent_size); 0 unset
    int crlf = -1;                   // end_of_line: -1 unset (or `cr`), 0 lf, 1 crlf
    QString charset;                 // utf-8, utf-8-bom, latin1; "" unset (utf-16 is not supported)
    int trimTrailingWhitespace = -1; // -1 unset, 0 false, 1 true
    int insertFinalNewline = -1;
    QStringList sources;             // the .editorconfig files with a section matching the file
    bool isEmpty() const { return sources.isEmpty(); }
};

Properties forFile(const QString &absolutePath);
// Whether a section glob matches `relativePath` ('/'-separated, relative to the .editorconfig's folder).
bool globMatches(const QString &glob, const QString &relativePath);

} // namespace EditorConfig
