#include "Emmet.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

namespace Emmet {
namespace {

struct Node {
    QString name; // empty = implied by the parent
    QString id;
    QStringList classes;
    QVector<QPair<QString, QString>> attrs; // value may be empty (-> tab stop) ; boolean attrs carry a null string
    QString text;
    bool hasText = false;
    bool group = false;
    int repeat = 1;
    QVector<Node> children;
};

const QSet<QString> &inlineTags()
{
    static const QSet<QString> s = {"a",     "abbr",  "acronym", "applet", "b",   "basefont", "bdo",    "big",  "br",  "button", "cite",
                                    "code",  "del",   "dfn",     "em",     "font", "i",       "img",    "input", "iframe", "ins", "kbd",
                                    "label", "map",   "object",  "q",      "s",    "samp",    "select", "small", "span", "strike", "strong",
                                    "sub",   "sup",   "textarea", "tt",    "u",    "var"};
    return s;
}

const QSet<QString> &voidTags()
{
    static const QSet<QString> s = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    return s;
}

// Short names: bq -> blockquote ...
const QHash<QString, QString> &aliases()
{
    static const QHash<QString, QString> h = {
        {"bq", "blockquote"}, {"btn", "button"},   {"fig", "figure"},   {"figc", "figcaption"}, {"ifr", "iframe"}, {"emb", "embed"},
        {"obj", "object"},    {"str", "strong"},   {"sect", "section"}, {"art", "article"},     {"hdr", "header"},  {"ftr", "footer"},
        {"adr", "address"},   {"dlg", "dialog"},   {"tarea", "textarea"}, {"opt", "option"},    {"optg", "optgroup"}, {"colg", "colgroup"},
        {"fst", "fieldset"},  {"fset", "fieldset"}, {"inp", "input"},   {"sel", "select"},      {"cap", "caption"}, {"det", "details"},
        {"sum", "summary"}};
    return h;
}

// Implied element name inside `parent`.
QString impliedName(const QString &parent)
{
    if (parent == QLatin1String("ul") || parent == QLatin1String("ol") || parent == QLatin1String("menu"))
        return QStringLiteral("li");
    if (parent == QLatin1String("table") || parent == QLatin1String("tbody") || parent == QLatin1String("thead") || parent == QLatin1String("tfoot"))
        return QStringLiteral("tr");
    if (parent == QLatin1String("tr"))
        return QStringLiteral("td");
    if (parent == QLatin1String("select") || parent == QLatin1String("optgroup") || parent == QLatin1String("datalist"))
        return QStringLiteral("option");
    if (parent == QLatin1String("dl"))
        return QStringLiteral("dt");
    if (inlineTags().contains(parent) || parent == QLatin1String("p"))
        return QStringLiteral("span");
    return QStringLiteral("div");
}

class Parser
{
public:
    explicit Parser(const QString &s) : m_s(s) {}

    bool parse(QVector<Node> *out)
    {
        *out = siblings();
        return !m_failed && m_i == m_s.size() && !out->isEmpty();
    }

private:
    QChar peek() const { return m_i < m_s.size() ? m_s.at(m_i) : QChar(); }
    bool atEnd() const { return m_i >= m_s.size(); }
    static bool isNameChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char(':') || c == QLatin1Char('$'); }

    QVector<Node> siblings()
    {
        QVector<Node> out;
        Node n = unit();
        while (!m_failed) {
            const QChar c = peek();
            if (c == QLatin1Char('+')) {
                ++m_i;
                out.append(n);
                n = unit();
            } else if (c == QLatin1Char('>')) {
                ++m_i;
                n.children = siblings();
            } else if (c == QLatin1Char('^')) {
                ++m_i;
                out.append(n);
                if (peek() == QLatin1Char('^'))
                    return out; // climb further: the caller handles the next '^'
                n = unit();
            } else {
                break;
            }
        }
        out.append(n);
        return out;
    }

    Node unit()
    {
        if (peek() == QLatin1Char('(')) {
            ++m_i;
            Node g;
            g.group = true;
            g.children = siblings();
            if (peek() != QLatin1Char(')')) {
                m_failed = true;
                return g;
            }
            ++m_i;
            multiplier(g);
            return g;
        }
        return element();
    }

    void multiplier(Node &n)
    {
        if (peek() != QLatin1Char('*'))
            return;
        ++m_i;
        int start = m_i;
        while (peek().isDigit())
            ++m_i;
        if (m_i == start || m_i - start > 3) {
            m_failed = true;
            return;
        }
        n.repeat = qMax(1, m_s.mid(start, m_i - start).toInt());
    }

    QString word()
    {
        int start = m_i;
        while (!atEnd() && isNameChar(peek()))
            ++m_i;
        return m_s.mid(start, m_i - start);
    }

    Node element()
    {
        Node n;
        bool any = false;
        if (peek().isLetter() || peek() == QLatin1Char('!')) {
            n.name = word();
            any = !n.name.isEmpty();
        }
        for (;;) {
            const QChar c = peek();
            if (c == QLatin1Char('#')) {
                ++m_i;
                n.id = word();
                any = true;
                if (n.id.isEmpty())
                    m_failed = true;
            } else if (c == QLatin1Char('.')) {
                ++m_i;
                const QString cls = word();
                if (cls.isEmpty())
                    m_failed = true;
                n.classes << cls;
                any = true;
            } else if (c == QLatin1Char('[')) {
                ++m_i;
                attributes(n);
                any = true;
            } else if (c == QLatin1Char('{')) {
                ++m_i;
                int depth = 1, start = m_i;
                while (!atEnd() && depth > 0) {
                    if (peek() == QLatin1Char('{'))
                        ++depth;
                    else if (peek() == QLatin1Char('}'))
                        --depth;
                    ++m_i;
                }
                if (depth != 0) {
                    m_failed = true;
                    return n;
                }
                n.text = m_s.mid(start, m_i - 1 - start);
                n.hasText = true;
                any = true;
            } else {
                break;
            }
            if (m_failed)
                return n;
        }
        multiplier(n);
        if (!any)
            m_failed = true;
        return n;
    }

    void attributes(Node &n)
    {
        while (!atEnd() && peek() != QLatin1Char(']')) {
            while (peek().isSpace())
                ++m_i;
            int start = m_i;
            while (!atEnd() && (isNameChar(peek()) || peek() == QLatin1Char('@')))
                ++m_i;
            const QString name = m_s.mid(start, m_i - start);
            if (name.isEmpty()) {
                m_failed = true;
                return;
            }
            QString value;
            bool boolean = true;
            if (peek() == QLatin1Char('=')) {
                ++m_i;
                boolean = false;
                if (peek() == QLatin1Char('"') || peek() == QLatin1Char('\'')) {
                    const QChar q = peek();
                    ++m_i;
                    const int vs = m_i;
                    while (!atEnd() && peek() != q)
                        ++m_i;
                    value = m_s.mid(vs, m_i - vs);
                    if (!atEnd())
                        ++m_i;
                } else {
                    const int vs = m_i;
                    while (!atEnd() && !peek().isSpace() && peek() != QLatin1Char(']'))
                        ++m_i;
                    value = m_s.mid(vs, m_i - vs);
                }
            }
            Q_UNUSED(boolean)
            n.attrs.append({name, value});
            while (peek().isSpace())
                ++m_i;
        }
        if (peek() != QLatin1Char(']'))
            m_failed = true;
        else
            ++m_i;
    }

    QString m_s;
    int m_i = 0;
    bool m_failed = false;
};

// Replaces runs of `$` with the 1-based index, zero padded to the run's width.
QString numbered(const QString &s, int index)
{
    if (!s.contains(QLatin1Char('$')))
        return s;
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) != QLatin1Char('$')) {
            out += s.at(i);
            continue;
        }
        int n = 0;
        while (i < s.size() && s.at(i) == QLatin1Char('$')) {
            ++n;
            ++i;
        }
        --i;
        out += QStringLiteral("%1").arg(index, n, 10, QLatin1Char('0'));
    }
    return out;
}

QString escapeSnippet(const QString &s)
{
    QString out;
    for (const QChar c : s) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('$') || c == QLatin1Char('}'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

class Renderer
{
public:
    Renderer(const Options &o) : m_o(o) {}

    QString render(const QVector<Node> &roots)
    {
        QString out;
        bool first = true;
        m_multiline = true;
        emitList(roots, QString(), 0, 1, out, first);
        return out;
    }
    int stops() const { return m_stop - 1; }

private:
    struct Resolved {
        QString name;
        bool isInline = false;
    };

    static bool hasBlockChild(const Node &n, const QString &parentName)
    {
        for (const Node &c : n.children) {
            if (c.group) {
                if (hasBlockChild(c, parentName))
                    return true;
                continue;
            }
            const QString name = c.name.isEmpty() ? impliedName(parentName) : c.name;
            if (!inlineTags().contains(resolveAlias(name)))
                return true;
        }
        return false;
    }

    static QString resolveAlias(const QString &name)
    {
        const QString base = name.section(QLatin1Char(':'), 0, 0);
        return aliases().value(base, base);
    }

    QString nl(int depth) const
    {
        QString s = QStringLiteral("\n");
        for (int i = 0; i < depth; ++i)
            s += m_o.indent;
        return s;
    }

    // Siblings go on their own lines when the container is multi-line; a group's children are siblings of the group.
    void emitList(const QVector<Node> &list, const QString &parentName, int depth, int index, QString &out, bool &first)
    {
        for (const Node &n : list)
            for (int i = 1; i <= n.repeat; ++i) {
                const int idx = n.repeat > 1 ? i : index;
                if (n.group) {
                    emitList(n.children, parentName, depth, idx, out, first);
                } else {
                    if (!first && m_multiline)
                        out += nl(depth);
                    first = false;
                    emitElement(n, parentName, depth, idx, out);
                }
            }
    }

    void emitElement(const Node &n, const QString &parentName, int depth, int index, QString &out)
    {
        if (n.name.isEmpty() && n.id.isEmpty() && n.classes.isEmpty() && n.attrs.isEmpty() && n.hasText) { // `{text}` alone
            out += escapeSnippet(numbered(n.text, index));
            return;
        }
        QString name = n.name.isEmpty() ? impliedName(parentName) : n.name;
        QString shortcut; // "input:text" -> type=text
        if (name.contains(QLatin1Char(':'))) {
            shortcut = name.section(QLatin1Char(':'), 1);
            name = name.section(QLatin1Char(':'), 0, 0);
        }
        name = aliases().value(name, name);
        name = numbered(name, index);
        const bool isVoid = voidTags().contains(name);

        // Attributes: id, class, explicit ones, then the tag's defaults for what is missing.
        QVector<QPair<QString, QString>> attrs;
        QString classes = numbered(n.classes.join(QLatin1Char(' ')), index);
        const QString id = numbered(n.id, index);
        if (!id.isEmpty())
            attrs.append({QStringLiteral("id"), id});
        if (!classes.isEmpty())
            attrs.append({m_o.jsx ? QStringLiteral("className") : QStringLiteral("class"), classes});
        QSet<QString> have;
        for (const auto &a : n.attrs)
            have.insert(a.first);
        auto addDefault = [&](const QString &k, const QString &v) {
            if (!have.contains(k)) {
                attrs.append({k, v});
                have.insert(k);
            }
        };
        if (name == QLatin1String("a") && shortcut == QLatin1String("mail"))
            addDefault(QStringLiteral("href"), QStringLiteral("mailto:"));
        else if (name == QLatin1String("a") && shortcut == QLatin1String("link"))
            addDefault(QStringLiteral("href"), QStringLiteral("http://"));
        if (name == QLatin1String("input") && !shortcut.isEmpty() && shortcut != QLatin1String("t"))
            addDefault(QStringLiteral("type"), shortcut == QLatin1String("c") ? QStringLiteral("checkbox") : shortcut == QLatin1String("r") ? QStringLiteral("radio") : shortcut);
        else if (name == QLatin1String("input") && (shortcut.isEmpty() || shortcut == QLatin1String("t")))
            addDefault(QStringLiteral("type"), QStringLiteral("text"));
        if (name == QLatin1String("link") && shortcut == QLatin1String("css")) {
            addDefault(QStringLiteral("rel"), QStringLiteral("stylesheet"));
            addDefault(QStringLiteral("href"), QStringLiteral(""));
        }
        if (name == QLatin1String("script") && shortcut == QLatin1String("src"))
            addDefault(QStringLiteral("src"), QStringLiteral(""));
        for (const auto &a : n.attrs) {
            QString key = a.first;
            if (m_o.jsx && key == QLatin1String("class"))
                key = QStringLiteral("className");
            if (m_o.jsx && key == QLatin1String("for"))
                key = QStringLiteral("htmlFor");
            attrs.append({key, a.second});
        }
        if (n.attrs.isEmpty() && shortcut.isEmpty()) { // implied attributes of common tags
            if (name == QLatin1String("a"))
                addDefault(QStringLiteral("href"), QStringLiteral(""));
            else if (name == QLatin1String("img")) {
                addDefault(QStringLiteral("src"), QStringLiteral(""));
                addDefault(QStringLiteral("alt"), QStringLiteral(""));
            } else if (name == QLatin1String("link")) {
                addDefault(QStringLiteral("rel"), QStringLiteral("stylesheet"));
                addDefault(QStringLiteral("href"), QStringLiteral(""));
            } else if (name == QLatin1String("form"))
                addDefault(QStringLiteral("action"), QStringLiteral(""));
            else if (name == QLatin1String("iframe") || name == QLatin1String("source"))
                addDefault(QStringLiteral("src"), QStringLiteral(""));
        }

        QString open = QStringLiteral("<") + escapeSnippet(name);
        for (const auto &a : attrs) {
            const QString key = numbered(a.first, index);
            const QString value = numbered(a.second, index);
            open += QLatin1Char(' ') + escapeSnippet(key) + QStringLiteral("=\"");
            open += value.isEmpty() ? QStringLiteral("$%1").arg(m_stop++) : escapeSnippet(value);
            open += QLatin1Char('"');
        }
        if (isVoid) {
            out += open + (m_o.jsx ? QStringLiteral(" />") : QStringLiteral(">"));
            return;
        }
        out += open + QLatin1Char('>');
        const QString text = n.hasText ? escapeSnippet(numbered(n.text, index)) : QString();
        const QString close = QStringLiteral("</") + escapeSnippet(name) + QLatin1Char('>');
        if (n.children.isEmpty()) {
            out += n.hasText ? text : QStringLiteral("$%1").arg(m_stop++);
            out += close;
            return;
        }
        if (n.hasText)
            out += text;
        const bool saved = m_multiline;
        bool first = true;
        if (hasBlockChild(n, name)) {
            m_multiline = true;
            out += nl(depth + 1);
            emitList(n.children, name, depth + 1, index, out, first);
            out += nl(depth);
        } else {
            m_multiline = false; // only inline children: keep them on the element's line
            emitList(n.children, name, depth + 1, index, out, first);
        }
        m_multiline = saved;
        out += close;
    }

    Options m_o;
    int m_stop = 1;
    bool m_multiline = true;
};

QString documentSkeleton(const Options &o)
{
    const QString i = o.indent;
    return QStringLiteral("<!DOCTYPE html>\n<html lang=\"${1:en}\">\n<head>\n") + i + QStringLiteral("<meta charset=\"UTF-8\">\n") + i +
           QStringLiteral("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n") + i +
           QStringLiteral("<title>${2:Document}</title>\n</head>\n<body>\n") + i + QStringLiteral("$0\n</body>\n</html>");
}

} // namespace

QString expand(const QString &abbreviation, const Options &options)
{
    const QString abbr = abbreviation.trimmed();
    if (abbr.isEmpty())
        return {};
    if (!options.jsx && (abbr == QLatin1String("!") || abbr == QLatin1String("html:5") || abbr == QLatin1String("doc")))
        return documentSkeleton(options);
    QVector<Node> roots;
    Parser parser(abbr);
    if (!parser.parse(&roots))
        return {};
    Renderer r(options);
    QString out = r.render(roots);
    if (out.isEmpty())
        return {};
    // The caret ends after the whole expansion unless a stop was placed there.
    return out + QStringLiteral("$0");
}

int findAbbreviation(const QString &line, int column, bool jsx)
{
    Q_UNUSED(jsx)
    column = qBound(0, column, int(line.size()));
    QVector<QChar> stack;
    int i = column;
    while (i > 0) {
        const QChar c = line.at(i - 1);
        if (!stack.isEmpty()) {
            if ((c == QLatin1Char('[') && stack.last() == QLatin1Char(']')) || (c == QLatin1Char('{') && stack.last() == QLatin1Char('}')) ||
                (c == QLatin1Char('(') && stack.last() == QLatin1Char(')')))
                stack.removeLast();
            else if ((c == QLatin1Char(']') || c == QLatin1Char('}') || c == QLatin1Char(')')) && stack.last() != QLatin1Char('}') &&
                     stack.last() != QLatin1Char(']'))
                stack.append(c); // nested parentheses
            --i;
            continue;
        }
        if (c == QLatin1Char(']') || c == QLatin1Char('}') || c == QLatin1Char(')')) {
            stack.append(c);
        } else if (c.isLetterOrNumber() || QStringLiteral(".#>+^*$@:-_!").contains(c)) {
            // part of the abbreviation
        } else if (c == QLatin1Char('[') || c == QLatin1Char('{') || c == QLatin1Char('(')) {
            break; // unbalanced opener
        } else {
            break; // whitespace, quote, '<', '=', ';' ...
        }
        --i;
    }
    if (!stack.isEmpty())
        return -1;
    // Trim what cannot start an abbreviation.
    while (i < column && QStringLiteral(">+^*-@:$").contains(line.at(i)))
        ++i;
    if (i >= column)
        return -1;
    if (i > 0 && line.at(i - 1) == QLatin1Char('<'))
        return -1; // inside a tag
    return i;
}

} // namespace Emmet
