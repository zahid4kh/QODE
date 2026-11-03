#pragma once

#include <QString>
#include <QTextBlock>
#include <QVector>

// Small text helpers shared by the CodeEditor*.cpp files (the editor's own: nothing else includes this header).
namespace CodeEditorText {

constexpr int kFoldWidth = 16;
constexpr int kBulbWidth = 14; // gutter room for the light bulb

inline bool isImportLine(const QTextBlock &b) { return b.text().startsWith(QLatin1String("import ")); }
inline bool isIdentifierChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

inline bool isOpeningBracket(QChar c)
{
    return c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{');
}

inline bool isBracket(QChar c)
{
    return isOpeningBracket(c) || c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}');
}

inline QChar partnerOf(QChar c)
{
    switch (c.unicode()) {
    case '(': return QLatin1Char(')');
    case ')': return QLatin1Char('(');
    case '[': return QLatin1Char(']');
    case ']': return QLatin1Char('[');
    case '{': return QLatin1Char('}');
    default: return QLatin1Char('{');
    }
}

// "3 min ago", "2 day(s) ago" ... for a Unix time.
QString relativeTime(qint64 secs);
// One flag per character: true inside a string or comment (as classified by the syntax highlighter).
// Empty when the block has no highlighting, meaning "all code".
QVector<bool> protectedMask(const QTextBlock &block);

} // namespace CodeEditorText
