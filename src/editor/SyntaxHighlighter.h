#pragma once

#include "Language.h"
#include "settings/Theme.h"

#include <QSyntaxHighlighter>

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
