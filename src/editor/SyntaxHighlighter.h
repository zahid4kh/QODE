#pragma once

#include "Language.h"
#include "settings/Theme.h"

#include <QSyntaxHighlighter>

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

protected:
    void highlightBlock(const QString &text) override;

private:
    QTextCharFormat formatFor(TokenRole role) const;
    void rebuildFormats();

    const LanguageDefinition *m_lang = nullptr;
    Theme m_theme;
    QHash<int, QTextCharFormat> m_formats;
};
