#pragma once

#include <QFont>
#include <QString>

struct Theme;

// Turns the Markdown a language server sends (hover, signature help, completion documentation) into the rich text the popups
// show: fenced code blocks are coloured with the editor's own syntax highlighter and theme, the rest goes through
// QTextDocument's Markdown reader.
namespace HoverRender {

// File-name suffix for a code fence's language id ("typescript" -> "ts"), "" when unknown.
QString extensionFor(const QString &language);

// `code` as a coloured block in the editor font. `language` is a fence id or an extension; unknown = plain.
QString codeBlock(const QString &code, const QString &language, const Theme &theme, const QFont &mono);
// The same, without the box: a run of coloured text for inline use (the active parameter inside a signature).
QString codeInline(const QString &code, const QString &language, const Theme &theme);

// Markdown to HTML. `defaultLanguage` colours fences that name none. The result is a fragment (no <html>/<body>).
QString markdownHtml(const QString &markdown, const Theme &theme, const QFont &mono, const QString &defaultLanguage = {});

} // namespace HoverRender
