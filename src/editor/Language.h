#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

// Semantic token classes; SyntaxHighlighter maps these to theme colours.
enum class TokenRole {
    Keyword, Type, String, Comment, Number, Preprocessor, Function, Tag, Attribute,
    Heading, Emphasis, Strong, Code, Link
};

// A single-line regex rule: the whole match (or capture group `group`) gets `role`.
struct HighlightRule {
    QRegularExpression pattern;
    TokenRole role;
    int group = 0;
};

// Text that "protects" its contents from ordinary rules: comments and strings.
// If `end` is valid the span may cross line boundaries.
struct DelimitedRule {
    QRegularExpression start;
    QRegularExpression end; // invalid => runs to end of line
    TokenRole role;
};

struct LanguageDefinition {
    QString name;
    QVector<HighlightRule> rules;      // applied first, in order (later wins)
    QVector<DelimitedRule> delimited;  // then these override; earliest match wins, ties by order
    QString lineComment;               // for future comment toggling / display
    bool csv = false;                  // delimited data: SyntaxHighlighter colours it column by column instead of using rules
    QChar csvDelimiter;                // fixed separator for csv (tab for TSV); null => detected from the first line
};

// Registry of supported languages. Add a language by writing a builder in Language.cpp
// and mapping its file extensions/names in forFile().
namespace Languages {

const LanguageDefinition *forFile(const QString &fileName); // nullptr => plain text
QString nameForFile(const QString &fileName);               // "Plain Text" when unknown

} // namespace Languages
