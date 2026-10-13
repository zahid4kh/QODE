#include "HoverRender.h"

#include "Language.h"
#include "SyntaxHighlighter.h"
#include "settings/Theme.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

namespace HoverRender {

QString extensionFor(const QString &language)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("typescript"), QStringLiteral("ts")},       {QStringLiteral("typescriptreact"), QStringLiteral("tsx")},
        {QStringLiteral("javascript"), QStringLiteral("js")},       {QStringLiteral("javascriptreact"), QStringLiteral("jsx")},
        {QStringLiteral("python"), QStringLiteral("py")},           {QStringLiteral("kotlin"), QStringLiteral("kt")},
        {QStringLiteral("rust"), QStringLiteral("rs")},             {QStringLiteral("c++"), QStringLiteral("cpp")},
        {QStringLiteral("cplusplus"), QStringLiteral("cpp")},       {QStringLiteral("bash"), QStringLiteral("sh")},
        {QStringLiteral("shell"), QStringLiteral("sh")},            {QStringLiteral("zsh"), QStringLiteral("sh")},
        {QStringLiteral("markdown"), QStringLiteral("md")},         {QStringLiteral("yaml"), QStringLiteral("yml")},
        {QStringLiteral("golang"), QStringLiteral("go")},
    };
    const QString l = language.trimmed().toLower();
    return map.value(l, l);
}

namespace {

QString escape(const QString &s)
{
    QString out = s.toHtmlEscaped();
    out.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return out;
}

// Runs of text with the syntax highlighter's colours, as <span>s. Newlines stay as '\n' (callers choose <br> or <pre>).
QString coloured(const QString &code, const QString &language, const Theme &theme, bool lineBreaks)
{
    const QString ext = extensionFor(language);
    const LanguageDefinition *def = ext.isEmpty() ? nullptr : Languages::forFile(QStringLiteral("x.") + ext);
    if (!def)
        return lineBreaks ? escape(code) : code.toHtmlEscaped();
    QTextDocument doc;
    auto *hl = new SyntaxHighlighter(&doc);
    hl->setTheme(theme);
    hl->setLanguage(def);
    doc.setPlainText(code);
    hl->rehighlight();
    QString out;
    bool first = true;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (!first)
            out += lineBreaks ? QStringLiteral("<br>") : QStringLiteral("\n");
        first = false;
        const QString text = b.text();
        int at = 0;
        auto plain = [&](int to) {
            if (to > at)
                out += text.mid(at, to - at).toHtmlEscaped();
            at = qMax(at, to);
        };
        QList<QTextLayout::FormatRange> ranges = b.layout()->formats();
        std::sort(ranges.begin(), ranges.end(), [](const QTextLayout::FormatRange &x, const QTextLayout::FormatRange &y) { return x.start < y.start; });
        for (const QTextLayout::FormatRange &r : std::as_const(ranges)) {
            if (r.start < at)
                continue;
            plain(r.start);
            QString style;
            if (r.format.foreground().style() != Qt::NoBrush)
                style += QStringLiteral("color:%1;").arg(r.format.foreground().color().name());
            if (r.format.fontWeight() >= QFont::Bold)
                style += QStringLiteral("font-weight:bold;");
            if (r.format.fontItalic())
                style += QStringLiteral("font-style:italic;");
            const QString piece = text.mid(r.start, r.length).toHtmlEscaped();
            out += style.isEmpty() ? piece : QStringLiteral("<span style=\"%1\">%2</span>").arg(style, piece);
            at = r.start + r.length;
        }
        plain(text.size());
    }
    return out;
}

} // namespace

QString codeInline(const QString &code, const QString &language, const Theme &theme)
{
    return coloured(code, language, theme, true);
}

QString codeBlock(const QString &code, const QString &language, const Theme &theme, const QFont &mono)
{
    QString body = coloured(code.trimmed(), language, theme, false);
    // A one-cell table is the one box Qt's rich text paints with a background.
    return QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"6\" bgcolor=\"%1\"><tr><td>"
                          "<pre style=\"margin:0; white-space:pre-wrap; font-family:'%2'; color:%3\">%4</pre></td></tr></table>")
        .arg(theme.currentLine.name(), mono.family(), theme.editorFg.name(), body);
}

QString markdownHtml(const QString &markdown, const Theme &theme, const QFont &mono, const QString &defaultLanguage)
{
    static const QRegularExpression fence(QStringLiteral(R"((?:^|\n)[ \t]*(`{3,}|~{3,})[ \t]*([^\n`]*)\n(.*?)\n[ \t]*\1[ \t]*(?=\n|$))"),
                                          QRegularExpression::DotMatchesEverythingOption);
    auto prose = [&](const QString &md) {
        if (md.trimmed().isEmpty())
            return QString();
        QTextDocument doc;
        doc.setMarkdown(md);
        QString html = doc.toHtml();
        const int bodyStart = html.indexOf(QStringLiteral("<body"));
        if (bodyStart >= 0) // keep the fonts of the popup, drop the page styling
            html = html.mid(html.indexOf(QLatin1Char('>'), bodyStart) + 1);
        html.remove(QStringLiteral("</body></html>"));
        // Inline `code` comes out in Qt's default monospace; use the editor font and the theme's colour.
        html.replace(QRegularExpression(QStringLiteral("font-family:'?(?:Courier New|monospace|Monospace)'?")),
                     QStringLiteral("font-family:'%1'").arg(mono.family()));
        return html;
    };
    QString out;
    int last = 0;
    for (auto it = fence.globalMatch(markdown); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += prose(markdown.mid(last, m.capturedStart() - last));
        QString lang = m.captured(2).trimmed().section(QRegularExpression(QStringLiteral("[\\s{]")), 0, 0);
        if (lang.isEmpty())
            lang = defaultLanguage;
        out += codeBlock(m.captured(3), lang, theme, mono);
        last = m.capturedEnd();
    }
    out += prose(markdown.mid(last));
    return out;
}

} // namespace HoverRender
