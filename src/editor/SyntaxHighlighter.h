#pragma once

#include "Language.h"
#include "settings/Theme.h"

#include <QSyntaxHighlighter>
#include <QVector>
#include <functional>

// Every format the highlighter applies carries its TokenRole under this property, so other code
// (bracket matching, folding) can tell strings and comments from real code.
constexpr int kTokenRoleProperty = QTextFormat::UserProperty + 1;

class SyntaxHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit SyntaxHighlighter(QTextDocument *doc);

    void setLanguage(const LanguageDefinition *lang);
    const LanguageDefinition *language() const { return m_lang; }
    void setTheme(const Theme &theme);
    // Extra advance after the given columns of a block, as (column, pixels): CodeEditor leaves room for colour swatches and
    // inlay hints this way (only formats set here, not extra selections, change the layout). Call rehighlightBlock() for
    // the blocks affected.
    using Gap = QPair<int, int>;
    void setGapProvider(std::function<QVector<Gap>(const QTextBlock &)> provider) { m_gaps = std::move(provider); }

    // Semantic highlighting: what the language server knows about each identifier (a parameter, a property, a class ...)
    // is painted over the regex colours. `hashes` holds qHash of each line's text when the tokens were computed; a line whose
    // text has changed since is left to the regex rules until the next list arrives. The blocks whose tokens differ from the
    // previous list are re-highlighted.
    struct SemanticToken {
        int column = 0, length = 0;
        TokenRole role = TokenRole::Variable;
        bool deprecated = false;
    };
    using SemanticLines = QVector<QVector<SemanticToken>>;
    void setSemanticTokens(const SemanticLines &lines, const QVector<uint> &hashes);
    bool hasSemanticTokens() const { return !m_semantic.isEmpty(); }
    // Lines were inserted (delta > 0) or removed after line `edited`: the stored tokens move with their text.
    void shiftSemanticLines(int edited, int delta);
    void clearSemanticTokens() { setSemanticTokens({}, {}); }
    static uint lineHash(const QString &text) { return qHash(text); }

protected:
    void highlightBlock(const QString &text) override;

private:
    void highlightText(const QString &text);
    void highlightCsv(const QString &text);
    QTextCharFormat formatFor(TokenRole role) const;
    void rebuildFormats();

    const LanguageDefinition *m_lang = nullptr;
    Theme m_theme;
    QHash<int, QTextCharFormat> m_formats;
    std::function<QVector<Gap>(const QTextBlock &)> m_gaps;
    SemanticLines m_semantic;
    QVector<uint> m_semanticHash;
    QChar m_csvDelimiter;
};
