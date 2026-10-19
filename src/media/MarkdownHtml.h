#pragma once

#include <QString>
#include <functional>

// Converts Markdown (CommonMark + GitHub tables, task lists, strikethrough, autolinks, reference links,
// setext headings, fenced / indented code, raw HTML) into the small HTML subset QTextBrowser renders.
// Qt Core only. Qt's own QTextDocument::setMarkdown() loses all text that follows raw HTML, which READMEs
// start with, so the preview uses this instead.
namespace MarkdownHtml {

// Optional: returns the HTML body for a code block's text in a given fence language (colour spans), or "" to leave it plain.
using Highlighter = std::function<QString(const QString &code, const QString &language)>;

QString convert(const QString &markdown, Highlighter highlight = {});

} // namespace MarkdownHtml
