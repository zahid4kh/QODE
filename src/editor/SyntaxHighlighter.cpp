#include "SyntaxHighlighter.h"

#include "CsvData.h"

#include <QFont>
#include <QRegularExpression>
#include <QTextBlock>
#include <QVarLengthArray>

#include <climits>

namespace {
// A line inside a heredoc has the block state kHeredocState + 4 * the terminator's index, + 1 for `<<-` (leading tabs
// before the terminator allowed), + 2 for a quoted terminator (no $expansion in the body). Delimited rules use 1..n.
constexpr int kHeredocState = 1 << 16;
} // namespace

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
    m_formats[int(TokenRole::Variable)] = fmt(m_theme.variable);
    m_formats[int(TokenRole::Property)] = fmt(m_theme.property);
    m_formats[int(TokenRole::Parameter)] = fmt(m_theme.parameter, false, true);
    m_formats[int(TokenRole::Constant)] = fmt(m_theme.constant);
    for (auto it = m_formats.begin(); it != m_formats.end(); ++it)
        it.value().setProperty(kTokenRoleProperty, it.key());
}

QTextCharFormat SyntaxHighlighter::formatFor(TokenRole role) const
{
    return m_formats.value(int(role));
}

void SyntaxHighlighter::setSemanticTokens(const SemanticLines &lines, const QVector<uint> &hashes)
{
    const SemanticLines old = std::move(m_semantic);
    const QVector<uint> oldHash = std::move(m_semanticHash);
    m_semantic = lines;
    m_semanticHash = hashes;

    auto same = [](const QVector<SemanticToken> &a, const QVector<SemanticToken> &b) {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i)
            if (a[i].column != b[i].column || a[i].length != b[i].length || a[i].role != b[i].role || a[i].deprecated != b[i].deprecated)
                return false;
        return true;
    };
    // The lines whose tokens (or the text they belong to) differ from what was painted before.
    QVector<int> changed;
    const int count = int(qMax(old.size(), m_semantic.size()));
    for (int n = 0; n < count; ++n) {
        const QVector<SemanticToken> none;
        const QVector<SemanticToken> &a = n < old.size() ? old[n] : none, &b = n < m_semantic.size() ? m_semantic[n] : none;
        const bool sameHash = (n < oldHash.size() ? oldHash[n] : 0u) == (n < m_semanticHash.size() ? m_semanticHash[n] : 0u);
        if (!same(a, b) || (!b.isEmpty() && !sameHash))
            changed << n;
    }
    if (changed.isEmpty())
        return;
    if (changed.size() > 300) {
        rehighlight();
        return;
    }
    for (const int n : std::as_const(changed)) {
        const QTextBlock b = document()->findBlockByNumber(n);
        if (b.isValid())
            rehighlightBlock(b);
    }
}

void SyntaxHighlighter::shiftSemanticLines(int edited, int delta)
{
    if (m_semantic.isEmpty() || delta == 0)
        return;
    const int at = edited + 1;
    if (at < 0 || at > m_semantic.size())
        return;
    if (delta > 0) {
        m_semantic.insert(at, delta, QVector<SemanticToken>());
        m_semanticHash.insert(qMin<int>(at, int(m_semanticHash.size())), delta, 0u);
    } else {
        m_semantic.remove(at, qMin<int>(-delta, int(m_semantic.size()) - at));
        m_semanticHash.remove(qMin<int>(at, int(m_semanticHash.size())), qMin<int>(-delta, qMax(0, int(m_semanticHash.size()) - at)));
    }
}

void SyntaxHighlighter::highlightBlock(const QString &text)
{
    // A line this long is minified or generated: it stays plain (each keystroke in it would run every rule over all of
    // it again) and passes the state it was given on, so a comment or string open before it goes on after it.
    if (text.size() > kMaxHighlightedLine) {
        setCurrentBlockState(qMax(previousBlockState(), 0));
        return;
    }
    highlightText(text);
    const int blockNumber = currentBlock().blockNumber();
    if (blockNumber < m_semantic.size() && blockNumber < m_semanticHash.size() && m_semanticHash[blockNumber] == lineHash(text)) {
        for (const SemanticToken &t : m_semantic[blockNumber]) {
            if (t.column < 0 || t.length <= 0 || t.column + t.length > text.size())
                continue;
            // Comments (KDoc references ...) stay as they are; strings may hold template expressions, which are code.
            const QVariant role = format(t.column).property(kTokenRoleProperty);
            if (role.isValid() && role.toInt() == int(TokenRole::Comment))
                continue;
            QTextCharFormat f = formatFor(t.role);
            if (t.deprecated)
                f.setFontStrikeOut(true);
            setFormat(t.column, t.length, f);
        }
    }
    if (!m_gaps)
        return;
    for (const Gap &gap : m_gaps(currentBlock())) {
        if (gap.first < 0 || gap.first >= text.size())
            continue;
        QTextCharFormat f = format(gap.first);
        f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        f.setFontLetterSpacing(gap.second);
        setFormat(gap.first, 1, f);
    }
}

void SyntaxHighlighter::highlightText(const QString &text)
{
    setCurrentBlockState(0);
    if (m_lang && m_lang->csv) {
        highlightCsv(text);
        return;
    }
    if (!m_lang || (text.isEmpty() && previousBlockState() <= 0))
        return;
    // PCRE2 validates the whole subject on every match() unless told not to, which made every match on a long line cost
    // the line's length: check it once per block instead.
    m_matchOptions = QStringView(text).isValidUtf16() ? QRegularExpression::DontCheckSubjectStringMatchOption
                                                      : QRegularExpression::NoMatchOption;

    if (m_lang->heredocs && previousBlockState() >= kHeredocState) {
        const int code = previousBlockState() - kHeredocState;
        QString line = text;
        if (code & 1)
            while (line.startsWith(QLatin1Char('\t')))
                line.remove(0, 1);
        if (line == m_heredocWords.value(code / 4)) {
            setFormat(0, text.size(), formatFor(TokenRole::Keyword));
            return;
        }
        setFormat(0, text.size(), formatFor(TokenRole::String));
        if (!(code & 2))
            interpolate(text, 0, text.size());
        setCurrentBlockState(previousBlockState());
        return;
    }

    // 1. Ordinary token rules.
    if (!text.isEmpty()) {
        for (const HighlightRule &r : m_lang->rules) {
            auto it = r.pattern.globalMatch(text, 0, QRegularExpression::NormalMatch, m_matchOptions);
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
        const auto m = d.end.match(text, 0, QRegularExpression::NormalMatch, m_matchOptions);
        if (!m.hasMatch()) {
            setFormat(0, text.length(), formatFor(d.role));
            if (d.interpolates)
                interpolate(text, 0, text.length());
            setCurrentBlockState(prev);
            return;
        }
        pos = m.capturedEnd();
        setFormat(0, pos, formatFor(d.role));
        if (d.interpolates)
            interpolate(text, 0, pos);
    }

    // Each delimiter's next start is remembered and only searched again once `pos` has passed it: searching every
    // delimiter from `pos` after each string made long lines quadratic (a 500 KB minified JSON line took seconds,
    // because a delimiter that never matches, like a comment in JSON, rescanned the rest of the line each time).
    struct Next {
        int start = -1, len = 0; // start -1: not searched yet; INT_MAX: no more matches on this line
    };
    QVarLengthArray<Next, 8> next(delims.size());
    while (pos < text.length()) {
        int bestStart = -1, bestLen = 0, bestIdx = -1;
        for (int i = 0; i < delims.size(); ++i) {
            Next &n = next[i];
            if (n.start < pos) {
                const auto m = delims[i].start.match(text, pos, QRegularExpression::NormalMatch, m_matchOptions);
                n.start = m.hasMatch() ? int(m.capturedStart()) : INT_MAX;
                n.len = int(m.capturedLength());
            }
            if (n.start != INT_MAX && (bestStart < 0 || n.start < bestStart)) {
                bestStart = n.start;
                bestLen = n.len;
                bestIdx = i;
            }
        }
        if (bestIdx < 0)
            break;
        const DelimitedRule &d = delims[bestIdx];
        if (!d.end.isValid() || d.end.pattern().isEmpty()) {
            setFormat(bestStart, qMax(bestLen, 1), formatFor(d.role));
            if (d.interpolates)
                interpolate(text, bestStart, bestStart + qMax(bestLen, 1));
            pos = bestStart + qMax(bestLen, 1);
            continue;
        }
        const int from = bestStart + qMax(bestLen, 1);
        const auto e = d.end.match(text, from, QRegularExpression::NormalMatch, m_matchOptions);
        if (!e.hasMatch()) {
            setFormat(bestStart, text.length() - bestStart, formatFor(d.role));
            if (d.interpolates)
                interpolate(text, from, text.length());
            setCurrentBlockState(bestIdx + 1);
            return;
        }
        setFormat(bestStart, e.capturedEnd() - bestStart, formatFor(d.role));
        if (d.interpolates)
            interpolate(text, from, e.capturedEnd());
        pos = e.capturedEnd();
    }
    if (m_lang->heredocs)
        openHeredoc(text);
}

// $var and ${…} inside a string keep the colour they have outside it.
void SyntaxHighlighter::interpolate(const QString &text, int from, int to)
{
    if (!m_lang->interpolation.isValid() || m_lang->interpolation.pattern().isEmpty())
        return;
    auto it = m_lang->interpolation.globalMatch(text, from, QRegularExpression::NormalMatch, m_matchOptions);
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.capturedStart() >= to)
            break;
        setFormat(m.capturedStart(), qMin(m.capturedEnd(), to) - m.capturedStart(), formatFor(m_lang->interpolationRole));
    }
}

// `<<EOF`, `<<-EOF`, `<<'EOF'`, `<<\EOF` outside strings and comments: the next lines are the heredoc body.
void SyntaxHighlighter::openHeredoc(const QString &text)
{
    static const QRegularExpression op(QStringLiteral("(?<!<)<<(-?)\\s*(?:(['\"])([A-Za-z_][\\w.-]*)\\2|(\\\\?)([A-Za-z_][\\w.-]*))"));
    auto it = op.globalMatch(text, 0, QRegularExpression::NormalMatch, m_matchOptions);
    while (it.hasNext()) {
        const auto m = it.next();
        const QVariant role = format(m.capturedStart()).property(kTokenRoleProperty);
        if (role.isValid() && (role.toInt() == int(TokenRole::String) || role.toInt() == int(TokenRole::Comment)))
            continue;
        const bool quoted = m.capturedLength(2) > 0 || m.capturedLength(4) > 0;
        const QString word = quoted && m.capturedLength(2) ? m.captured(3) : m.captured(5);
        int index = int(m_heredocWords.indexOf(word));
        if (index < 0) {
            index = int(m_heredocWords.size());
            m_heredocWords << word;
        }
        setFormat(m.capturedStart(), m.capturedLength(), formatFor(TokenRole::Keyword));
        setCurrentBlockState(kHeredocState + 4 * index + (m.capturedLength(1) ? 1 : 0) + (quoted ? 2 : 0));
        return;
    }
}

// Every column gets its own colour so a row can be read across; the first row (the header) is bold. The block state
// is 1 + the column of a quoted field that is still open at the end of the line (fields may contain line breaks).
void SyntaxHighlighter::highlightCsv(const QString &text)
{
    static const TokenRole columnRoles[] = {TokenRole::Type,     TokenRole::String,   TokenRole::Number,
                                            TokenRole::Function, TokenRole::Keyword, TokenRole::Attribute};
    constexpr int roleCount = sizeof(columnRoles) / sizeof(columnRoles[0]);
    constexpr int maxColumn = 4000;

    const int prev = previousBlockState();
    const bool header = currentBlock().blockNumber() == 0;
    if (header || m_csvDelimiter.isNull())
        m_csvDelimiter = m_lang->csvDelimiter.isNull() ? CsvData::detectDelimiter(document()->firstBlock().text())
                                                       : m_lang->csvDelimiter;
    const QChar delim = m_csvDelimiter;

    auto fmtFor = [&](int column) {
        QTextCharFormat f = formatFor(columnRoles[column % roleCount]);
        if (header)
            f.setFontWeight(QFont::Bold);
        return f;
    };

    int column = prev > 0 ? prev - 1 : 0;
    int i = 0;
    const int n = text.size();
    bool inQuote = prev > 0;
    int fieldStart = 0;
    for (; i < n; ++i) {
        const QChar c = text.at(i);
        if (inQuote) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < n && text.at(i + 1) == QLatin1Char('"'))
                    ++i; // doubled quote inside the field
                else
                    inQuote = false;
            }
            continue;
        }
        if (c == QLatin1Char('"') && i == fieldStart) {
            inQuote = true;
        } else if (c == delim) {
            if (i > fieldStart)
                setFormat(fieldStart, i - fieldStart, fmtFor(column));
            setFormat(i, 1, formatFor(TokenRole::Comment));
            fieldStart = i + 1;
            column = qMin(column + 1, maxColumn);
        }
    }
    if (n > fieldStart)
        setFormat(fieldStart, n - fieldStart, fmtFor(column));
    setCurrentBlockState(inQuote ? 1 + column : 0);
}
