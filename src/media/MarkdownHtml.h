#pragma once

#include <QString>

// Converts Markdown (CommonMark + GitHub tables, task lists, strikethrough, autolinks, reference links,
// setext headings, fenced / indented code, raw HTML) into the small HTML subset QTextBrowser renders.
// Qt Core only. Qt's own QTextDocument::setMarkdown() loses all text that follows raw HTML, which READMEs
// start with, so the preview uses this instead.
namespace MarkdownHtml {

QString convert(const QString &markdown);

} // namespace MarkdownHtml
