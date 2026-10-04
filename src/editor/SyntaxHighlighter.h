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
    // Extra advance after the given columns of a block (CodeEditor leaves room for colour swatches this way: only formats
    // set here, not extra selections, change the layout). Call rehighlightBlock() for the blocks affected.
    void setGapProvider(std::function<QVector<int>(const QTextBlock &)> provider, qreal pixels)
    {
        m_gaps = std::move(provider);
        m_gapPixels = pixels;
    }

protected:
    void highlightBlock(const QString &text) override;

private:
    void highlightText(const QString &text);
    QTextCharFormat formatFor(TokenRole role) const;
    void rebuildFormats();

    const LanguageDefinition *m_lang = nullptr;
    Theme m_theme;
    QHash<int, QTextCharFormat> m_formats;
    std::function<QVector<int>(const QTextBlock &)> m_gaps;
    qreal m_gapPixels = 0;
};
