#include "MarkdownHtml.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

namespace MarkdownHtml {
namespace {

QString esc(const QString &s)
{
    QString r;
    r.reserve(s.size() + 8);
    for (const QChar c : s) {
        if (c == QLatin1Char('&'))
            r += QStringLiteral("&amp;");
        else if (c == QLatin1Char('<'))
            r += QStringLiteral("&lt;");
        else if (c == QLatin1Char('>'))
            r += QStringLiteral("&gt;");
        else if (c == QLatin1Char('"'))
            r += QStringLiteral("&quot;");
        else
            r += c;
    }
    return r;
}

bool isPunct(QChar c)
{
    return c.unicode() < 128 && c.isPunct() ? true : QStringLiteral("$+<=>^`|~").contains(c);
}

struct Ref {
    QString url, title;
};

struct Ctx {
    QHash<QString, Ref> refs;
    QStringList atoms; // finished HTML fragments, referenced from inline text by a placeholder
    QString placeholder(const QString &html)
    {
        atoms.append(html);
        return QString(QChar(0xE000)) + QString::number(atoms.size() - 1) + QChar(0xE001);
    }
};

QString normLabel(const QString &s) { return s.simplified().toLower(); }

QString cleanUrl(QString u)
{
    u = u.trimmed();
    if (u.startsWith(QLatin1Char('<')) && u.endsWith(QLatin1Char('>')))
        u = u.mid(1, u.size() - 2);
    u.replace(QLatin1Char(' '), QStringLiteral("%20"));
    if (u.startsWith(QLatin1String("javascript:"), Qt::CaseInsensitive))
        return QString();
    return u;
}

bool isRemote(const QString &u)
{
    return u.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || u.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        || u.startsWith(QLatin1String("//")) || u.startsWith(QLatin1String("data:"), Qt::CaseInsensitive);
}

// A picture we cannot fetch (remote) shows its alt text, a local one is an <img>.
QString imageHtml(const QString &src, const QString &alt, const QString &title, const QString &extra = QString())
{
    const QString u = cleanUrl(src);
    if (u.isEmpty() || isRemote(u))
        return alt.trimmed().isEmpty() ? QString() : QStringLiteral("<i>[%1]</i>").arg(esc(alt));
    return QStringLiteral("<img src=\"%1\" alt=\"%2\"%3%4>")
        .arg(esc(u), esc(alt), title.isEmpty() ? QString() : QStringLiteral(" title=\"%1\"").arg(esc(title)), extra);
}

QString plainOf(const QString &html)
{
    QString t = html;
    t.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    t.replace(QStringLiteral("&amp;"), QStringLiteral("&")).replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return t;
}

const QSet<QString> &allowedTags()
{
    static const QSet<QString> s = {
        QStringLiteral("p"), QStringLiteral("div"), QStringLiteral("span"), QStringLiteral("a"), QStringLiteral("img"), QStringLiteral("br"),
        QStringLiteral("hr"), QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
        QStringLiteral("h6"), QStringLiteral("b"), QStringLiteral("strong"), QStringLiteral("i"), QStringLiteral("em"), QStringLiteral("u"),
        QStringLiteral("s"), QStringLiteral("del"), QStringLiteral("strike"), QStringLiteral("code"), QStringLiteral("pre"), QStringLiteral("kbd"),
        QStringLiteral("sub"), QStringLiteral("sup"), QStringLiteral("table"), QStringLiteral("thead"), QStringLiteral("tbody"), QStringLiteral("tr"),
        QStringLiteral("th"), QStringLiteral("td"), QStringLiteral("ul"), QStringLiteral("ol"), QStringLiteral("li"), QStringLiteral("blockquote"),
        QStringLiteral("center"), QStringLiteral("font")};
    return s;
}

// One HTML tag from the source, reduced to what the preview renders. Returns "" for tags it drops.
QString cleanTag(Ctx &, const QString &tag)
{
    static const QRegularExpression tagRe(QStringLiteral("^<(/?)([A-Za-z][A-Za-z0-9-]*)([^>]*?)(/?)>$"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression attrRe(QStringLiteral("([A-Za-z_:][\\w:.-]*)\\s*(?:=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'>]+)))?"));
    const auto m = tagRe.match(tag);
    if (!m.hasMatch())
        return QString();
    const bool closing = !m.captured(1).isEmpty();
    QString name = m.captured(2).toLower();
    if (name == QLatin1String("summary"))
        return closing ? QStringLiteral("</b></p>") : QStringLiteral("<p><b>");
    if (name == QLatin1String("details") || name == QLatin1String("figure") || name == QLatin1String("section") || name == QLatin1String("article")
        || name == QLatin1String("header") || name == QLatin1String("footer") || name == QLatin1String("main") || name == QLatin1String("nav")
        || name == QLatin1String("aside") || name == QLatin1String("figcaption") || name == QLatin1String("picture"))
        return closing ? QStringLiteral("</div>") : QStringLiteral("<div>");
    if (!allowedTags().contains(name))
        return QString();
    if (closing)
        return name == QLatin1String("br") || name == QLatin1String("img") || name == QLatin1String("hr") ? QString() : QStringLiteral("</%1>").arg(name);

    static const QSet<QString> okAttrs = {QStringLiteral("align"), QStringLiteral("href"), QStringLiteral("src"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("alt"), QStringLiteral("title"), QStringLiteral("colspan"), QStringLiteral("rowspan"),
        QStringLiteral("name"), QStringLiteral("valign"), QStringLiteral("color"), QStringLiteral("start")};
    QString attrs, src, alt;
    auto it = attrRe.globalMatch(m.captured(3));
    while (it.hasNext()) {
        const auto a = it.next();
        const QString an = a.captured(1).toLower();
        QString av = a.captured(2);
        if (av.isEmpty())
            av = a.captured(3);
        if (av.isEmpty())
            av = a.captured(4);
        if (!okAttrs.contains(an))
            continue;
        if (name == QLatin1String("img") && an == QLatin1String("src")) {
            src = av;
            continue;
        }
        if (name == QLatin1String("img") && an == QLatin1String("alt"))
            alt = av;
        if (an == QLatin1String("href")) {
            av = cleanUrl(av);
            if (av.isEmpty())
                continue;
        }
        attrs += QStringLiteral(" %1=\"%2\"").arg(an, esc(av).replace(QLatin1String("&amp;"), QLatin1String("&amp;")));
    }
    if (name == QLatin1String("img")) {
        const QString u = cleanUrl(src);
        if (u.isEmpty() || isRemote(u))
            return alt.trimmed().isEmpty() ? QString() : QStringLiteral("<i>[%1]</i>").arg(esc(alt));
        return QStringLiteral("<img src=\"%1\"%2>").arg(esc(u), attrs);
    }
    if (name == QLatin1String("br") || name == QLatin1String("hr"))
        return QStringLiteral("<%1%2>").arg(name, attrs);
    return QStringLiteral("<%1%2>").arg(name, attrs);
}

// Raw HTML (a block or an inline run): keep the tags we render, drop the rest, drop script-like content.
QString cleanHtml(Ctx &ctx, QString html)
{
    static const QRegularExpression dropContent(QStringLiteral("<(script|style|iframe|object|embed|svg|video|audio|canvas)\\b[\\s\\S]*?</\\1\\s*>"),
                                                QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression comment(QStringLiteral("<!--[\\s\\S]*?-->"));
    static const QRegularExpression tag(QStringLiteral("<[^<>]*>"));
    html.remove(comment);
    html.remove(dropContent);
    QString out;
    int last = 0;
    auto it = tag.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        out += cleanTag(ctx, m.captured());
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

// ---------------------------------------------------------------- inline

QString inlineHtml(Ctx &ctx, const QString &src);

// Finds the `]` that closes the `[` at `open`; -1 if unbalanced.
int closeBracket(const QString &s, int open)
{
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == QLatin1Char('\\')) {
            ++i;
        } else if (c == QLatin1Char('`')) {
            int n = 0;
            while (i + n < s.size() && s[i + n] == QLatin1Char('`'))
                ++n;
            const int e = s.indexOf(QString(n, QLatin1Char('`')), i + n);
            if (e >= 0)
                i = e + n - 1;
            else
                i += n - 1;
        } else if (c == QLatin1Char('[')) {
            ++depth;
        } else if (c == QLatin1Char(']')) {
            if (--depth == 0)
                return i;
        }
    }
    return -1;
}

// Parses "(dest "title")" at `pos` (which holds '('). Returns the index after ')' or -1.
int parseDest(const QString &s, int pos, QString &url, QString &title)
{
    int i = pos + 1;
    while (i < s.size() && s[i].isSpace())
        ++i;
    int start = i;
    if (i < s.size() && s[i] == QLatin1Char('<')) {
        const int e = s.indexOf(QLatin1Char('>'), i);
        if (e < 0)
            return -1;
        url = s.mid(i + 1, e - i - 1);
        i = e + 1;
    } else {
        int depth = 0;
        for (; i < s.size(); ++i) {
            const QChar c = s[i];
            if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                ++i;
            } else if (c == QLatin1Char('(')) {
                ++depth;
            } else if (c == QLatin1Char(')')) {
                if (depth == 0)
                    break;
                --depth;
            } else if (c.isSpace()) {
                break;
            }
        }
        url = s.mid(start, i - start);
    }
    while (i < s.size() && s[i].isSpace())
        ++i;
    title.clear();
    if (i < s.size() && (s[i] == QLatin1Char('"') || s[i] == QLatin1Char('\'') || s[i] == QLatin1Char('('))) {
        const QChar q = s[i] == QLatin1Char('(') ? QLatin1Char(')') : s[i];
        const int e = s.indexOf(q, i + 1);
        if (e < 0)
            return -1;
        title = s.mid(i + 1, e - i - 1);
        i = e + 1;
        while (i < s.size() && s[i].isSpace())
            ++i;
    }
    if (i >= s.size() || s[i] != QLatin1Char(')'))
        return -1;
    return i + 1;
}

QString linkHtml(const QString &url, const QString &title, const QString &inner)
{
    const QString u = cleanUrl(url);
    if (u.isEmpty())
        return inner;
    return QStringLiteral("<a href=\"%1\"%2>%3</a>").arg(esc(u), title.isEmpty() ? QString() : QStringLiteral(" title=\"%1\"").arg(esc(title)), inner);
}

QString inlineHtml(Ctx &ctx, const QString &s)
{
    static const QRegularExpression autolink(QStringLiteral("^<([A-Za-z][A-Za-z0-9+.-]{1,31}:[^\\s<>]*)>"));
    static const QRegularExpression emailLink(QStringLiteral("^<([^\\s<>@]+@[^\\s<>@]+\\.[^\\s<>@]+)>"));
    static const QRegularExpression htmlTag(QStringLiteral("^(?:<!--[\\s\\S]*?-->|</?[A-Za-z][A-Za-z0-9-]*(?:\\s+[^<>]*)?/?>)"));
    static const QRegularExpression entity(QStringLiteral("^&(?:#\\d{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});"));
    static const QRegularExpression bareUrl(QStringLiteral("^(?:https?://|www\\.)[^\\s<>]+"));

    QString out;
    const int n = s.size();
    int i = 0;
    while (i < n) {
        const QChar c = s[i];
        if (c == QLatin1Char('\\') && i + 1 < n) {
            if (s[i + 1] == QLatin1Char('\n')) {
                out += ctx.placeholder(QStringLiteral("<br>"));
                i += 2;
                continue;
            }
            if (isPunct(s[i + 1])) {
                out += ctx.placeholder(esc(QString(s[i + 1])));
                i += 2;
                continue;
            }
        }
        if (c == QLatin1Char('`')) {
            int run = 0;
            while (i + run < n && s[i + run] == QLatin1Char('`'))
                ++run;
            int e = i + run;
            int close = -1;
            while ((e = s.indexOf(QLatin1Char('`'), e)) >= 0) {
                int r = 0;
                while (e + r < n && s[e + r] == QLatin1Char('`'))
                    ++r;
                if (r == run) {
                    close = e;
                    break;
                }
                e += r;
            }
            if (close >= 0) {
                QString code = s.mid(i + run, close - i - run).replace(QLatin1Char('\n'), QLatin1Char(' '));
                if (code.size() > 2 && code.startsWith(QLatin1Char(' ')) && code.endsWith(QLatin1Char(' ')) && code.trimmed() == code.mid(1, code.size() - 2))
                    code = code.mid(1, code.size() - 2);
                out += ctx.placeholder(QStringLiteral("<code>\u2009%1\u2009</code>").arg(esc(code)));
                i = close + run;
                continue;
            }
            out += QString(run, c);
            i += run;
            continue;
        }
        if (c == QLatin1Char('<')) {
            const QString rest = s.mid(i, 2048);
            auto m = autolink.match(rest);
            if (m.hasMatch()) {
                out += ctx.placeholder(linkHtml(m.captured(1), QString(), esc(m.captured(1))));
                i += m.capturedLength();
                continue;
            }
            m = emailLink.match(rest);
            if (m.hasMatch()) {
                out += ctx.placeholder(linkHtml(QStringLiteral("mailto:") + m.captured(1), QString(), esc(m.captured(1))));
                i += m.capturedLength();
                continue;
            }
            m = htmlTag.match(rest);
            if (m.hasMatch()) {
                out += ctx.placeholder(cleanHtml(ctx, m.captured()));
                i += m.capturedLength();
                continue;
            }
            out += QStringLiteral("&lt;");
            ++i;
            continue;
        }
        const bool image = c == QLatin1Char('!') && i + 1 < n && s[i + 1] == QLatin1Char('[');
        if (c == QLatin1Char('[') || image) {
            const int open = image ? i + 1 : i;
            const int close = closeBracket(s, open);
            if (close > 0) {
                const QString text = s.mid(open + 1, close - open - 1);
                QString url, title;
                int next = -1;
                bool found = false;
                if (close + 1 < n && s[close + 1] == QLatin1Char('(')) {
                    next = parseDest(s, close + 1, url, title);
                    found = next > 0;
                }
                if (!found) { // [text][label], [text][] or [label]
                    QString label = text;
                    int after = close + 1;
                    int j = after;
                    while (j < n && s[j] == QLatin1Char(' '))
                        ++j;
                    if (j < n && s[j] == QLatin1Char('[')) {
                        const int e = s.indexOf(QLatin1Char(']'), j);
                        if (e > 0) {
                            const QString l = s.mid(j + 1, e - j - 1);
                            if (!l.isEmpty())
                                label = l;
                            after = e + 1;
                        }
                    }
                    const auto r = ctx.refs.constFind(normLabel(label));
                    if (r != ctx.refs.constEnd()) {
                        url = r->url;
                        title = r->title;
                        next = after;
                        found = true;
                    }
                }
                if (found) {
                    if (image)
                        out += ctx.placeholder(imageHtml(url, plainOf(inlineHtml(ctx, text)), title));
                    else
                        out += ctx.placeholder(linkHtml(url, title, inlineHtml(ctx, text)));
                    i = next;
                    continue;
                }
            }
            out += c;
            ++i;
            continue;
        }
        if (c == QLatin1Char('&')) {
            const auto m = entity.match(s.mid(i, 40));
            if (m.hasMatch()) {
                out += m.captured();
                i += m.capturedLength();
                continue;
            }
            out += QStringLiteral("&amp;");
            ++i;
            continue;
        }
        if ((c == QLatin1Char('h') || c == QLatin1Char('w')) && (i == 0 || !s[i - 1].isLetterOrNumber())) {
            auto m = bareUrl.match(s.mid(i, 2048));
            if (m.hasMatch()) {
                QString u = m.captured();
                while (!u.isEmpty() && QStringLiteral(".,;:!?'\"*_~").contains(u.back()))
                    u.chop(1);
                if (u.endsWith(QLatin1Char(')')) && u.count(QLatin1Char('(')) < u.count(QLatin1Char(')')))
                    u.chop(1);
                if (u.size() > 8) {
                    out += ctx.placeholder(linkHtml(u.startsWith(QLatin1String("www.")) ? QStringLiteral("https://") + u : u, QString(), esc(u)));
                    i += u.size();
                    continue;
                }
            }
        }
        if (c == QLatin1Char(' ')) { // two trailing spaces before a newline: hard break
            int j = i;
            while (j < n && s[j] == QLatin1Char(' '))
                ++j;
            if (j < n && s[j] == QLatin1Char('\n') && j - i >= 2) {
                out += ctx.placeholder(QStringLiteral("<br>"));
                i = j + 1;
                continue;
            }
        }
        if (c == QLatin1Char('<'))
            out += QStringLiteral("&lt;");
        else if (c == QLatin1Char('>'))
            out += QStringLiteral("&gt;");
        else
            out += c;
        ++i;
    }

    // Emphasis on the text between the placeholders (they hold no markers).
    struct Pass {
        QRegularExpression re;
        const char *open;
        const char *close;
    };
    static const QVector<Pass> passes = {
        {QRegularExpression(QStringLiteral("~~(?=\\S)([\\s\\S]+?)(?<=\\S)~~")), "<s>", "</s>"},
        {QRegularExpression(QStringLiteral("(?<![\\w*])\\*{3}(?=[^\\s*])([\\s\\S]+?)(?<=[^\\s*])\\*{3}(?!\\*)")), "<b><i>", "</i></b>"},
        {QRegularExpression(QStringLiteral("(?<![\\w_])_{3}(?=[^\\s_])([\\s\\S]+?)(?<=[^\\s_])_{3}(?![\\w_])")), "<b><i>", "</i></b>"},
        {QRegularExpression(QStringLiteral("(?<!\\*)\\*{2}(?=[^\\s*])([\\s\\S]+?)(?<=[^\\s*])\\*{2}(?!\\*)")), "<b>", "</b>"},
        {QRegularExpression(QStringLiteral("(?<![\\w_])__(?=[^\\s_])([\\s\\S]+?)(?<=[^\\s_])__(?![\\w_])")), "<b>", "</b>"},
        {QRegularExpression(QStringLiteral("(?<![\\w*])\\*(?=[^\\s*])([\\s\\S]+?)(?<=[^\\s*])\\*(?!\\*)")), "<i>", "</i>"},
        {QRegularExpression(QStringLiteral("(?<![\\w_])_(?=[^\\s_])([\\s\\S]+?)(?<=[^\\s_])_(?![\\w_])")), "<i>", "</i>"},
    };
    for (const Pass &p : passes)
        out.replace(p.re, QString::fromLatin1(p.open) + QStringLiteral("\\1") + QString::fromLatin1(p.close));

    // Put the finished fragments back (they may nest: a link's text is itself a fragment).
    static const QRegularExpression ph(QStringLiteral("\\x{E000}(\\d+)\\x{E001}"));
    for (int guard = 0; guard < 8 && out.contains(QChar(0xE000)); ++guard) {
        QString res;
        int last = 0;
        auto it = ph.globalMatch(out);
        while (it.hasNext()) {
            const auto m = it.next();
            res += out.mid(last, m.capturedStart() - last);
            const int idx = m.captured(1).toInt();
            res += idx >= 0 && idx < ctx.atoms.size() ? ctx.atoms[idx] : QString();
            last = m.capturedEnd();
        }
        res += out.mid(last);
        out = res;
    }
    return out;
}

// ---------------------------------------------------------------- blocks

int indentOf(const QString &l)
{
    int n = 0;
    while (n < l.size() && l[n] == QLatin1Char(' '))
        ++n;
    return n;
}

bool isBlank(const QString &l) { return l.trimmed().isEmpty(); }

const QRegularExpression &fenceRe()
{
    static const QRegularExpression r(QStringLiteral("^( {0,3})(`{3,}|~{3,})\\s*([^`]*)$"));
    return r;
}
const QRegularExpression &atxRe()
{
    static const QRegularExpression r(QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+(.*?))?(?:[ \\t]+#+)?[ \\t]*$"));
    return r;
}
const QRegularExpression &hrRe()
{
    static const QRegularExpression r(QStringLiteral("^ {0,3}([-*_])(?:[ \\t]*\\1){2,}[ \\t]*$"));
    return r;
}
const QRegularExpression &listRe()
{
    static const QRegularExpression r(QStringLiteral("^( {0,3})([-*+]|\\d{1,9}[.)])( +|$)(.*)$"));
    return r;
}
const QRegularExpression &tableSepRe()
{
    static const QRegularExpression r(QStringLiteral("^\\s*\\|?\\s*:?-+:?\\s*(?:\\|\\s*:?-+:?\\s*)*\\|?\\s*$"));
    return r;
}
const QRegularExpression &htmlBlockRe()
{
    static const QRegularExpression r(QStringLiteral("^ {0,3}(?:<!--|</?(?:p|div|h[1-6]|table|thead|tbody|tr|td|th|ul|ol|li|pre|blockquote|center|hr|details|summary|"
                                                      "section|article|header|footer|nav|aside|main|figure|figcaption|picture|img|a|br|video|iframe|script|style|"
                                                      "span|b|i|em|strong|kbd|sub|sup|code|font)(?:\\s|/?>|$))"),
                                      QRegularExpression::CaseInsensitiveOption);
    return r;
}

// A code box: a borderless table (cellpadding 7 marks it for MarkdownPreview, which paints the rounded
// background) with a header row holding the language and a Copy link, then the code itself.
QString codeBlock(const QString &code, const QString &lang)
{
    QString l = lang;
    l.remove(QRegularExpression(QStringLiteral("^[{.]|[}]$")));
    return QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"7\" width=\"100%\"><tr><td><small>%1</small></td>"
                          "<td align=\"right\"><small><a href=\"qode-copy:\">Copy</a></small></td></tr>"
                          "<tr><td colspan=\"2\"><pre>%2</pre></td></tr></table>")
        .arg(esc(l.toUpper()), esc(code));
}

QString blocks(Ctx &ctx, const QStringList &lines, bool tight);

// Does this line begin a block that may interrupt a paragraph?
bool interrupts(const QString &l)
{
    if (fenceRe().match(l).hasMatch() || atxRe().match(l).hasMatch() || hrRe().match(l).hasMatch())
        return true;
    if (l.trimmed().startsWith(QLatin1Char('>')) && indentOf(l) < 4)
        return true;
    const auto m = listRe().match(l);
    if (m.hasMatch() && !m.captured(4).trimmed().isEmpty() && (!m.captured(2)[0].isDigit() || m.captured(2).startsWith(QLatin1Char('1'))))
        return true;
    return false;
}

QStringList splitRow(QString row)
{
    row = row.trimmed();
    if (row.startsWith(QLatin1Char('|')))
        row.remove(0, 1);
    if (row.endsWith(QLatin1Char('|')) && !row.endsWith(QLatin1String("\\|")))
        row.chop(1);
    QStringList cells;
    QString cur;
    bool code = false;
    for (int i = 0; i < row.size(); ++i) {
        const QChar c = row[i];
        if (c == QLatin1Char('\\') && i + 1 < row.size() && row[i + 1] == QLatin1Char('|')) {
            cur += QLatin1Char('|');
            ++i;
        } else if (c == QLatin1Char('`')) {
            code = !code;
            cur += c;
        } else if (c == QLatin1Char('|') && !code) {
            cells << cur.trimmed();
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells << cur.trimmed();
    return cells;
}

QString tableHtml(Ctx &ctx, const QStringList &rows)
{
    const QStringList head = splitRow(rows[0]);
    QStringList aligns;
    for (const QString &c : splitRow(rows[1])) {
        const bool l = c.startsWith(QLatin1Char(':')), r = c.endsWith(QLatin1Char(':'));
        aligns << (l && r ? QStringLiteral(" align=\"center\"") : r ? QStringLiteral(" align=\"right\"") : QStringLiteral(" align=\"left\""));
    }
    auto cell = [&](const char *tag, const QString &text, int col) {
        return QStringLiteral("<%1%2>%3</%1>").arg(QLatin1String(tag), col < aligns.size() ? aligns[col] : QString(), inlineHtml(ctx, text));
    };
    QString h = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"6\" width=\"100%\"><tr>");
    for (int c = 0; c < head.size(); ++c)
        h += cell("th", head[c], c);
    h += QStringLiteral("</tr>");
    for (int r = 2; r < rows.size(); ++r) {
        const QStringList cells = splitRow(rows[r]);
        h += QStringLiteral("<tr>");
        for (int c = 0; c < head.size(); ++c)
            h += cell("td", c < cells.size() ? cells[c] : QString(), c);
        h += QStringLiteral("</tr>");
    }
    return h + QStringLiteral("</table>");
}

QString listHtml(Ctx &ctx, const QStringList &lines, int &i)
{
    const auto first = listRe().match(lines[i]);
    const int baseIndent = first.captured(1).size();
    const bool ordered = first.captured(2)[0].isDigit();
    const int start = ordered ? first.captured(2).left(first.captured(2).size() - 1).toInt() : 1;
    QVector<QStringList> items;
    bool loose = false;
    int offset = 0;
    bool prevBlank = false;
    const int n = lines.size();
    while (i < n) {
        const QString &line = lines[i];
        if (isBlank(line)) {
            int j = i + 1;
            while (j < n && isBlank(lines[j]))
                ++j;
            if (j >= n)
                break;
            const auto m = listRe().match(lines[j]);
            const int ind = indentOf(lines[j]);
            const bool sibling = m.hasMatch() && ind <= baseIndent + 1 && m.captured(2)[0].isDigit() == ordered;
            if (ind <= baseIndent && !sibling)
                break;
            if (sibling)
                loose = true;
            else if (!items.isEmpty())
                items.last() << QString();
            prevBlank = true;
            i = j;
            continue;
        }
        const auto m = listRe().match(line);
        const int ind = indentOf(line);
        if (m.hasMatch() && ind <= baseIndent + 1 && !hrRe().match(line).hasMatch() && (items.isEmpty() || m.captured(2)[0].isDigit() == ordered)) {
            const int spaces = m.captured(3).size();
            offset = m.captured(1).size() + m.captured(2).size() + (spaces > 4 || spaces == 0 ? 1 : spaces);
            items.append(QStringList{m.captured(4)});
        } else if (ind > baseIndent || (m.hasMatch() && ind > baseIndent)) {
            if (items.isEmpty())
                break;
            if (prevBlank) {
                // blank line then more content inside the item: the list is loose when it is not a nested list
                if (!listRe().match(line.mid(qMin(ind, offset))).hasMatch())
                    loose = true;
            }
            items.last() << line.mid(qMin(ind, offset));
        } else if (!prevBlank && !interrupts(line) && !items.isEmpty() && !items.last().isEmpty() && !isBlank(items.last().last())) {
            items.last() << line.trimmed(); // lazy continuation
        } else {
            break;
        }
        prevBlank = false;
        ++i;
    }

    QString h = ordered ? (start != 1 ? QStringLiteral("<ol start=\"%1\">").arg(start) : QStringLiteral("<ol>")) : QStringLiteral("<ul>");
    for (QStringList item : items) {
        while (!item.isEmpty() && isBlank(item.last()))
            item.removeLast();
        QString prefix;
        static const QRegularExpression task(QStringLiteral("^\\[([ xX])\\][ \\t]+"));
        if (!item.isEmpty()) {
            const auto t = task.match(item[0]);
            if (t.hasMatch()) {
                prefix = t.captured(1) == QLatin1String(" ") ? QStringLiteral("\u2610 ") : QStringLiteral("\u2611 ");
                item[0] = item[0].mid(t.capturedLength());
            }
        }
        h += QStringLiteral("<li>") + prefix + blocks(ctx, item, !loose) + QStringLiteral("</li>");
    }
    return h + (ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>"));
}

QString blocks(Ctx &ctx, const QStringList &srcLines, bool tight)
{
    QStringList lines;
    for (QString l : srcLines) {
        int t = 0;
        while (t < l.size() && l[t] == QLatin1Char('\t'))
            ++t;
        if (t)
            l = QString(t * 4, QLatin1Char(' ')) + l.mid(t);
        lines << l;
    }
    QString out;
    const int n = lines.size();
    int i = 0;
    bool lastTightPara = false; // tight list items separate paragraphs with a line break, not margins
    auto para = [&](const QString &html) {
        if (!tight)
            out += QStringLiteral("<p>") + html + QStringLiteral("</p>");
        else
            out += (lastTightPara ? QStringLiteral("<br>") : QString()) + html;
        lastTightPara = tight;
    };
    while (i < n) {
        const QString &line = lines[i];
        if (!isBlank(line) && !(indentOf(line) < 4 && !fenceRe().match(line).hasMatch() && !atxRe().match(line).hasMatch() && !hrRe().match(line).hasMatch()
                                && !line.trimmed().startsWith(QLatin1Char('>')) && !htmlBlockRe().match(line).hasMatch() && !listRe().match(line).hasMatch()))
            lastTightPara = false;
        if (isBlank(line)) {
            ++i;
            continue;
        }
        // fenced code
        auto fm = fenceRe().match(line);
        if (fm.hasMatch()) {
            const QChar fc = fm.captured(2)[0];
            const int flen = fm.captured(2).size();
            const int fin = fm.captured(1).size();
            QStringList code;
            ++i;
            while (i < n) {
                const QString t = lines[i].trimmed();
                if (indentOf(lines[i]) < 4 && t.size() >= flen && t == QString(t.size(), fc)) {
                    ++i;
                    break;
                }
                code << lines[i].mid(qMin(indentOf(lines[i]), fin));
                ++i;
            }
            out += codeBlock(code.join(QLatin1Char('\n')), fm.captured(3).trimmed().split(QLatin1Char(' ')).value(0));
            continue;
        }
        // indented code
        if (indentOf(line) >= 4) {
            QStringList code;
            while (i < n && (indentOf(lines[i]) >= 4 || isBlank(lines[i]))) {
                code << (isBlank(lines[i]) ? QString() : lines[i].mid(4));
                ++i;
            }
            while (!code.isEmpty() && code.last().isEmpty())
                code.removeLast();
            out += codeBlock(code.join(QLatin1Char('\n')), QString());
            continue;
        }
        // headings
        auto hm = atxRe().match(line);
        if (hm.hasMatch()) {
            const int lvl = hm.captured(1).size();
            const QString html = inlineHtml(ctx, hm.captured(2).trimmed());
            out += QStringLiteral("<h%1>%2</h%1>").arg(lvl).arg(html);
            ++i;
            continue;
        }
        if (hrRe().match(line).hasMatch()) {
            out += QStringLiteral("<hr>");
            ++i;
            continue;
        }
        // block quote
        if (line.trimmed().startsWith(QLatin1Char('>'))) {
            QStringList inner;
            while (i < n && !isBlank(lines[i])) {
                QString l = lines[i];
                const int p = l.indexOf(QLatin1Char('>'));
                if (p >= 0 && p < 4 && l.left(p).trimmed().isEmpty()) {
                    l = l.mid(p + 1);
                    if (l.startsWith(QLatin1Char(' ')))
                        l.remove(0, 1);
                    inner << l;
                } else if (!interrupts(l)) {
                    inner << l; // lazy continuation
                } else {
                    break;
                }
                ++i;
            }
            // keep empty quote lines (">") as paragraph separators
            out += QStringLiteral("<blockquote>") + blocks(ctx, inner, false) + QStringLiteral("</blockquote>");
            continue;
        }
        // raw HTML block
        if (htmlBlockRe().match(line).hasMatch()) {
            QStringList raw;
            const bool comment = line.trimmed().startsWith(QLatin1String("<!--"));
            while (i < n) {
                raw << lines[i];
                const bool endsComment = comment && lines[i].contains(QLatin1String("-->"));
                ++i;
                if (endsComment || (!comment && (i >= n || isBlank(lines[i]))))
                    break;
            }
            out += cleanHtml(ctx, raw.join(QLatin1Char('\n')));
            continue;
        }
        // list
        if (listRe().match(line).hasMatch()) {
            out += listHtml(ctx, lines, i);
            continue;
        }
        // table
        if (line.contains(QLatin1Char('|')) && i + 1 < n && lines[i + 1].contains(QLatin1Char('-')) && tableSepRe().match(lines[i + 1]).hasMatch()
            && splitRow(lines[i]).size() == splitRow(lines[i + 1]).size()) {
            QStringList rows{lines[i], lines[i + 1]};
            i += 2;
            while (i < n && !isBlank(lines[i]) && !interrupts(lines[i])) {
                rows << lines[i];
                ++i;
            }
            out += tableHtml(ctx, rows);
            continue;
        }
        // paragraph (or setext heading)
        QStringList text;
        int setext = 0;
        while (i < n && !isBlank(lines[i])) {
            if (!text.isEmpty()) {
                static const QRegularExpression underline(QStringLiteral("^ {0,3}(=+|-+)[ \\t]*$"));
                const auto u = underline.match(lines[i]);
                if (u.hasMatch()) {
                    setext = u.captured(1)[0] == QLatin1Char('=') ? 1 : 2;
                    ++i;
                    break;
                }
                if (interrupts(lines[i]) || htmlBlockRe().match(lines[i]).hasMatch())
                    break;
            }
            {
                const QString &raw = lines[i];
                const QString t = raw.trimmed();
                text << (raw.endsWith(QLatin1String("  ")) ? t + QStringLiteral("  ") : t);
            }
            ++i;
        }
        const QString html = inlineHtml(ctx, text.join(QLatin1Char('\n')));
        if (setext)
            out += QStringLiteral("<h%1>%2</h%1>").arg(setext).arg(html);
        else
            para(html);
    }
    return out;
}

} // namespace

QString convert(const QString &markdown)
{
    Ctx ctx;
    QStringList lines = markdown.split(QLatin1Char('\n'));
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
    if (!lines.isEmpty() && lines[0].startsWith(QChar(0xFEFF)))
        lines[0].remove(0, 1);

    // YAML front matter is metadata, not content.
    if (lines.size() > 2 && lines[0].trimmed() == QLatin1String("---")) {
        for (int j = 1; j < qMin(lines.size(), 200); ++j)
            if (lines[j].trimmed() == QLatin1String("---") || lines[j].trimmed() == QLatin1String("...")) {
                bool yaml = true;
                for (int k = 1; k < j; ++k)
                    if (!isBlank(lines[k]) && !lines[k].contains(QLatin1Char(':')) && !lines[k].trimmed().startsWith(QLatin1Char('-')) && !lines[k].startsWith(QLatin1Char(' '))
                        && !lines[k].startsWith(QLatin1Char('#')))
                        yaml = false;
                if (yaml)
                    lines = lines.mid(j + 1);
                break;
            }
    }

    // Reference definitions are collected first (outside code fences) and removed.
    static const QRegularExpression def(QStringLiteral("^ {0,3}\\[([^\\]]+)\\]:\\s*<?([^\\s>]+)>?(?:\\s+(?:\"(.*)\"|'(.*)'|\\((.*)\\)))?\\s*$"));
    QStringList kept;
    QString fence;
    for (const QString &l : lines) {
        const auto fm = fenceRe().match(l);
        if (fm.hasMatch()) {
            const QString f = fm.captured(2);
            if (fence.isEmpty())
                fence = f;
            else if (f[0] == fence[0] && f.size() >= fence.size() && fm.captured(3).trimmed().isEmpty())
                fence.clear();
        }
        if (fence.isEmpty() && !fm.hasMatch()) {
            const auto m = def.match(l);
            if (m.hasMatch()) {
                QString title = m.captured(3);
                if (title.isEmpty())
                    title = m.captured(4);
                if (title.isEmpty())
                    title = m.captured(5);
                if (!ctx.refs.contains(normLabel(m.captured(1))))
                    ctx.refs.insert(normLabel(m.captured(1)), {m.captured(2), title});
                continue;
            }
        }
        kept << l;
    }
    return blocks(ctx, kept, false);
}

} // namespace MarkdownHtml
