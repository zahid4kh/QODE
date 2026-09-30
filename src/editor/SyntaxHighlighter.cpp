#include "SyntaxHighlighter.h"

#include <QFont>

SyntaxHighlighter::SyntaxHighlighter(QTextDocument *doc)
    : QSyntaxHighlighter(doc)
{
    rebuildFormats();
}

void SyntaxHighlighter::setLanguage(const LanguageDefinition *lang)
{
    m_lang = lang;
    rehighlight();
}

void SyntaxHighlighter::setTheme(const Theme &theme)
{
    m_theme = theme;
    rebuildFormats();
    rehighlight();
}

void SyntaxHighlighter::rebuildFormats()
{
    auto fmt = [](const QColor &c, bool bold = false, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)
            f.setFontWeight(QFont::Bold);
        f.setFontItalic(italic);
        return f;
    };
    m_formats.clear();
    m_formats[int(TokenRole::Keyword)] = fmt(m_theme.keyword);
    m_formats[int(TokenRole::Type)] = fmt(m_theme.type);
    m_formats[int(TokenRole::String)] = fmt(m_theme.string);
    m_formats[int(TokenRole::Comment)] = fmt(m_theme.comment, false, true);
    m_formats[int(TokenRole::Number)] = fmt(m_theme.number);
    m_formats[int(TokenRole::Preprocessor)] = fmt(m_theme.preprocessor);
    m_formats[int(TokenRole::Function)] = fmt(m_theme.function);
    m_formats[int(TokenRole::Tag)] = fmt(m_theme.tag);
    m_formats[int(TokenRole::Attribute)] = fmt(m_theme.attribute);
    m_formats[int(TokenRole::Heading)] = fmt(m_theme.function, true);
    m_formats[int(TokenRole::Emphasis)] = fmt(m_theme.type, false, true);
    m_formats[int(TokenRole::Strong)] = fmt(m_theme.number, true);
    m_formats[int(TokenRole::Code)] = fmt(m_theme.string);
    m_formats[int(TokenRole::Link)] = fmt(m_theme.accent);
}

QTextCharFormat SyntaxHighlighter::formatFor(TokenRole role) const
{
    return m_formats.value(int(role));
}

void SyntaxHighlighter::highlightBlock(const QString &text)
{
    setCurrentBlockState(0);
    if (!m_lang || (text.isEmpty() && previousBlockState() <= 0))
        return;

    // 1. Ordinary token rules.
    if (!text.isEmpty()) {
        for (const HighlightRule &r : m_lang->rules) {
            auto it = r.pattern.globalMatch(text);
            while (it.hasNext()) {
                const auto m = it.next();
                const int s = m.capturedStart(r.group);
                const int l = m.capturedLength(r.group);
                if (s >= 0 && l > 0)
                    setFormat(s, l, formatFor(r.role));
            }
        }
    }

    // 2. Comments and strings override whatever the rules found inside them.
    //    State n>0 means "inside delimited rule n-1 carried over from the previous line".
    const auto &delims = m_lang->delimited;
    int pos = 0;
    const int prev = previousBlockState();
    if (prev > 0 && prev <= delims.size()) {
        const DelimitedRule &d = delims[prev - 1];
        const auto m = d.end.match(text, 0);
        if (!m.hasMatch()) {
            setFormat(0, text.length(), formatFor(d.role));
            setCurrentBlockState(prev);
            return;
        }
        pos = m.capturedEnd();
        setFormat(0, pos, formatFor(d.role));
    }

    while (pos < text.length()) {
        int bestStart = -1, bestLen = 0, bestIdx = -1;
        QRegularExpressionMatch bestMatch;
        for (int i = 0; i < delims.size(); ++i) {
            const auto m = delims[i].start.match(text, pos);
            if (m.hasMatch() && (bestStart < 0 || m.capturedStart() < bestStart)) {
                bestStart = m.capturedStart();
                bestLen = m.capturedLength();
                bestIdx = i;
                bestMatch = m;
            }
        }
        if (bestIdx < 0)
            break;
        const DelimitedRule &d = delims[bestIdx];
        if (!d.end.isValid() || d.end.pattern().isEmpty()) {
            setFormat(bestStart, qMax(bestLen, 1), formatFor(d.role));
            pos = bestStart + qMax(bestLen, 1);
            continue;
        }
        const int from = bestStart + qMax(bestLen, 1);
        const auto e = d.end.match(text, from);
        if (!e.hasMatch()) {
            setFormat(bestStart, text.length() - bestStart, formatFor(d.role));
            setCurrentBlockState(bestIdx + 1);
            return;
        }
        setFormat(bestStart, e.capturedEnd() - bestStart, formatFor(d.role));
        pos = e.capturedEnd();
    }
}
