#include "CppTools.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace CppTools {

namespace {

QString tr(const char *text) { return QCoreApplication::translate("CppTools", text); }

const QStringList &headerExts()
{
    static const QStringList exts{QStringLiteral("h"),   QStringLiteral("hpp"), QStringLiteral("hh"),  QStringLiteral("hxx"),
                                  QStringLiteral("h++"), QStringLiteral("inl"), QStringLiteral("ipp"), QStringLiteral("tpp")};
    return exts;
}

const QStringList &sourceExts()
{
    static const QStringList exts{QStringLiteral("cpp"), QStringLiteral("cc"), QStringLiteral("cxx"), QStringLiteral("c"),
                                  QStringLiteral("c++"), QStringLiteral("m"),  QStringLiteral("mm")};
    return exts;
}

QString suffixOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    return dot > slash ? path.mid(dot + 1).toLower() : QString();
}

QString baseOf(const QString &path)
{
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? name.left(dot) : name;
}

bool isIdent(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

// "my-widget.h" -> "MY_WIDGET_H"
QString guardWord(const QString &text)
{
    QString out;
    for (const QChar c : text)
        out += c.isLetterOrNumber() ? c.toUpper() : QLatin1Char('_');
    return out;
}

// Header files for learning the project's guard style: the new file's own folder first, then a breadth-first walk of the
// project that skips hidden, build and dependency folders.
QStringList sampleHeaders(const QString &folder, const QString &root, int limit)
{
    QStringList out;
    QStringList queue{folder};
    if (!root.isEmpty() && root != folder)
        queue << root;
    QSet<QString> seen;
    int visited = 0;
    static const QStringList skip{QStringLiteral("build"), QStringLiteral("node_modules"), QStringLiteral("third_party"),
                                  QStringLiteral("3rdparty"), QStringLiteral("vendor"), QStringLiteral("external")};
    while (!queue.isEmpty() && out.size() < limit && visited < 400) {
        const QString dir = queue.takeFirst();
        if (seen.contains(dir))
            continue;
        seen.insert(dir);
        ++visited;
        const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &fi : entries) {
            if (fi.fileName().startsWith(QLatin1Char('.')))
                continue;
            if (fi.isDir()) {
                if (!skip.contains(fi.fileName()) && !fi.fileName().startsWith(QLatin1String("build-")) &&
                    !fi.fileName().startsWith(QLatin1String("cmake-build")))
                    queue << fi.absoluteFilePath();
            } else if (fi.size() > 0 && headerExts().mid(0, 5).contains(fi.suffix().toLower())) {
                out << fi.absoluteFilePath();
                if (out.size() >= limit)
                    break;
            }
        }
    }
    return out;
}

// --- Scanning C++ text ------------------------------------------------------------------------------------------------------

// Comments, the contents of string and character literals and preprocessor lines become spaces (line breaks stay), so the
// structure can be scanned without tripping over braces in them. Positions do not change.
QString blanked(const QString &text)
{
    QString b = text;
    const int n = text.size();
    const QChar space(QLatin1Char(' '));
    auto blank = [&](int i) {
        if (b[i] != QLatin1Char('\n'))
            b[i] = space;
    };
    bool lineStart = true;
    int i = 0;
    while (i < n) {
        const QChar c = text[i];
        if (c == QLatin1Char('\n')) {
            lineStart = true;
            ++i;
            continue;
        }
        if (lineStart && c.isSpace()) {
            ++i;
            continue;
        }
        if (lineStart && c == QLatin1Char('#')) { // a directive, with its continuation lines
            while (i < n && text[i] != QLatin1Char('\n')) {
                if (text[i] == QLatin1Char('\\') && i + 1 < n && text[i + 1] == QLatin1Char('\n')) {
                    b[i] = space;
                    i += 2;
                    continue;
                }
                b[i++] = space;
            }
            continue;
        }
        lineStart = false;
        if (c == QLatin1Char('/') && i + 1 < n && text[i + 1] == QLatin1Char('/')) {
            while (i < n && text[i] != QLatin1Char('\n'))
                b[i++] = space;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && text[i + 1] == QLatin1Char('*')) {
            const int end = text.indexOf(QLatin1String("*/"), i + 2);
            const int stop = end < 0 ? n : end + 2;
            for (; i < stop; ++i)
                blank(i);
            continue;
        }
        if (c == QLatin1Char('"') && i > 0 && text[i - 1] == QLatin1Char('R')) { // R"delim( ... )delim"
            const int open = text.indexOf(QLatin1Char('('), i);
            if (open > 0) {
                const QString closer = QLatin1Char(')') + text.mid(i + 1, open - i - 1) + QLatin1Char('"');
                const int end = text.indexOf(closer, open);
                const int stop = end < 0 ? n : end + closer.size() - 1;
                for (++i; i < stop; ++i)
                    blank(i);
                ++i;
                continue;
            }
        }
        if (c == QLatin1Char('\'') && i > 0 && text[i - 1].isDigit() && i + 1 < n && text[i + 1].isLetterOrNumber()) {
            ++i; // digit separator: 1'000'000
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            ++i;
            while (i < n && text[i] != c && text[i] != QLatin1Char('\n')) {
                if (text[i] == QLatin1Char('\\') && i + 1 < n) {
                    blank(i++);
                }
                blank(i++);
            }
            ++i; // the closing quote stays
            continue;
        }
        ++i;
    }
    return b;
}

// The partner of the bracket at `open` ('(', '[', '{' or '<'), counting only that kind; -1 when unmatched before `limit`.
int matching(const QString &b, int open, int limit = -1)
{
    const QChar o = b[open];
    const QChar c = o == QLatin1Char('(') ? QLatin1Char(')') : o == QLatin1Char('[') ? QLatin1Char(']')
                  : o == QLatin1Char('{') ? QLatin1Char('}')
                                          : QLatin1Char('>');
    const int end = limit < 0 ? b.size() : qMin(limit, int(b.size()));
    int depth = 0;
    for (int i = open; i < end; ++i) {
        if (b[i] == o)
            ++depth;
        else if (b[i] == c && --depth == 0)
            return i;
    }
    return -1;
}

// Splits at commas that are not nested in (), [], {} or <>.
QStringList splitTopLevel(const QString &s)
{
    QStringList out;
    int depth = 0, start = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{') || c == QLatin1Char('<'))
            ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}') || c == QLatin1Char('>'))
            depth = qMax(0, depth - 1);
        else if (c == QLatin1Char(',') && depth == 0) {
            out << s.mid(start, i - start);
            start = i + 1;
        }
    }
    if (!s.trimmed().isEmpty())
        out << s.mid(start);
    return out;
}

// "int a = 3" -> "int a": cuts at the first '=' that is not nested (default arguments, template defaults).
QString withoutDefault(const QString &param)
{
    int depth = 0;
    for (int i = 0; i < param.size(); ++i) {
        const QChar c = param[i];
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{') || c == QLatin1Char('<'))
            ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}') || c == QLatin1Char('>'))
            depth = qMax(0, depth - 1);
        else if (c == QLatin1Char('=') && depth == 0 && (i + 1 >= param.size() || param[i + 1] != QLatin1Char('=')))
            return param.left(i).simplified();
    }
    return param.simplified();
}

QString joinParams(const QString &params)
{
    QStringList list;
    for (const QString &p : splitTopLevel(params))
        list << withoutDefault(p);
    return list.join(QStringLiteral(", "));
}

// The parameter's type without its name and spaces ("const QString &name" -> "constQString&"), to compare overloads.
QString typeKey(const QString &param)
{
    static const QSet<QString> builtin{QStringLiteral("int"),    QStringLiteral("char"),     QStringLiteral("short"),
                                       QStringLiteral("long"),   QStringLiteral("double"),   QStringLiteral("float"),
                                       QStringLiteral("bool"),   QStringLiteral("unsigned"), QStringLiteral("signed"),
                                       QStringLiteral("void"),   QStringLiteral("auto"),     QStringLiteral("wchar_t"),
                                       QStringLiteral("char8_t"), QStringLiteral("char16_t"), QStringLiteral("char32_t"),
                                       QStringLiteral("const"),  QStringLiteral("volatile")};
    QString p = withoutDefault(param);
    int arr = p.indexOf(QLatin1Char('['));
    QString suffix;
    if (arr >= 0) {
        suffix = p.mid(arr);
        p = p.left(arr).trimmed();
    }
    int e = p.size();
    int s = e;
    while (s > 0 && isIdent(p[s - 1]))
        --s;
    if (s < e && s > 0) {
        const QString last = p.mid(s, e - s);
        const QString before = p.left(s).trimmed();
        if (!builtin.contains(last) && !before.isEmpty() && !before.endsWith(QLatin1String("::")))
            p = before;
    }
    p += suffix;
    p.remove(QLatin1Char(' '));
    return p;
}

QStringList typeKeys(const QString &params)
{
    QStringList out;
    for (const QString &p : splitTopLevel(params))
        out << typeKey(p);
    if (out.size() == 1 && (out[0] == QLatin1String("void") || out[0].isEmpty()))
        out.clear();
    return out;
}

// "template <typename T, int N = 3>" -> the parameter list without defaults, and the names "T, N".
void templateParams(const QString &inside, QString *declared, QString *names)
{
    QStringList decl, used;
    for (const QString &p : splitTopLevel(inside)) {
        const QString d = withoutDefault(p);
        decl << d;
        int e = d.size();
        while (e > 0 && !isIdent(d[e - 1]))
            --e;
        int s = e;
        while (s > 0 && isIdent(d[s - 1]))
            --s;
        QString name = d.mid(s, e - s);
        if (d.contains(QLatin1String("...")))
            name += QStringLiteral("...");
        used << name;
    }
    *declared = decl.join(QStringLiteral(", "));
    *names = used.join(QStringLiteral(", "));
}

const QRegularExpression &namespaceHead()
{
    static const QRegularExpression re(QStringLiteral("^(?:inline\\s+)?namespace(?:\\s+([\\w:]+(?:\\s*::\\s*(?:inline\\s+)?\\w+)*))?(?:\\s*\\[\\[.*\\]\\])?$"));
    return re;
}

bool isExternBlock(const QString &head)
{
    static const QRegularExpression re(QStringLiteral("^extern\\s*\"\\s*\"$"));
    return re.match(head).hasMatch();
}

struct Scope {
    bool isClass = false;
    QStringList names;    // namespace parts, or the class name (with template arguments)
    QString templateLine; // class templates: "template <typename T>"
    int open = -1, close = -1;
};

// A class head ("template <class T> class EXPORT Box : public Base"): name with template arguments and template line.
bool classHead(const QString &head, Scope *scope)
{
    QString rest = head;
    QString declared, names;
    bool templated = false;
    if (rest.startsWith(QLatin1String("template"))) {
        const int open = rest.indexOf(QLatin1Char('<'));
        const int close = open < 0 ? -1 : matching(rest, open);
        if (close < 0)
            return false;
        templateParams(rest.mid(open + 1, close - open - 1), &declared, &names);
        templated = true;
        rest = rest.mid(close + 1).trimmed();
    }
    static const QRegularExpression re(
        QStringLiteral("^(?:class|struct|union)\\s+(?:\\[\\[[^\\]]*\\]\\]\\s*)?(?:alignas\\s*\\([^)]*\\)\\s*)?(?:[A-Z_][A-Z0-9_]*\\s+)*(\\w+)\\s*(<[^{]*?>)?(?:\\s+final)?\\s*(?::.*)?$"));
    const QRegularExpressionMatch m = re.match(rest);
    if (!m.hasMatch())
        return false;
    scope->isClass = true;
    QString name = m.captured(1);
    if (!m.captured(2).isEmpty())
        name += m.captured(2).simplified(); // a specialisation keeps its own arguments
    else if (templated && !names.isEmpty())
        name += QLatin1Char('<') + names + QLatin1Char('>');
    scope->names = {name};
    if (templated && !declared.isEmpty())
        scope->templateLine = QStringLiteral("template <%1>").arg(declared);
    return true;
}

struct Statement {
    int start = 0, end = 0; // [start, end): the text before the ';'
    bool signal = false;     // declared under signals: / Q_SIGNALS:
};

struct Parsed {
    bool ok = false;
    QString error, name, prefix, params, trailing, memberTemplate;
};

// A declaration statement (simplified, from the blanked text) taken apart: return type and specifiers, name, parameters, the
// qualifiers after them. `className` is the innermost class (constructors have no return type).
Parsed parseStatement(QString s, const QString &className)
{
    Parsed p;
    p.error = tr("Put the caret on a function declaration");
    static const QRegularExpression lead(QStringLiteral("^(?:\\[\\[[^\\]]*\\]\\]|Q_[A-Z_]+(?:\\s*\\([^()]*\\))?|[A-Z][A-Z0-9_]*_EXPORT)\\s*"));
    for (QRegularExpressionMatch m = lead.match(s); m.hasMatch() && m.capturedLength() > 0; m = lead.match(s))
        s = s.mid(m.capturedLength());
    if (s.startsWith(QLatin1String("template"))) {
        const int open = s.indexOf(QLatin1Char('<'));
        const int close = open < 0 ? -1 : matching(s, open);
        if (close < 0)
            return p;
        QString declared, names;
        templateParams(s.mid(open + 1, close - open - 1), &declared, &names);
        p.memberTemplate = QStringLiteral("template <%1>").arg(declared);
        s = s.mid(close + 1).trimmed();
    }
    static const QRegularExpression notFunction(QStringLiteral("^(?:typedef|using|static_assert|return|friend)\\b"));
    if (notFunction.match(s).hasMatch()) {
        if (s.startsWith(QLatin1String("friend")))
            p.error = tr("A friend declaration has no definition of its own here");
        return p;
    }
    int nameStart = -1, paren = -1;
    static const QRegularExpression op(QStringLiteral("\\boperator\\b"));
    const QRegularExpressionMatch opm = op.match(s);
    if (opm.hasMatch()) {
        nameStart = opm.capturedStart();
        int k = opm.capturedEnd();
        while (k < s.size() && s[k] == QLatin1Char(' '))
            ++k;
        if (s.mid(k).startsWith(QLatin1String("()")))
            k += 2;
        paren = s.indexOf(QLatin1Char('('), k);
        if (paren < 0)
            return p;
        p.name = s.mid(nameStart, paren - nameStart).trimmed();
    } else {
        int angle = 0;
        for (int k = 0; k < s.size(); ++k) {
            const QChar c = s[k];
            if (c == QLatin1Char('<'))
                ++angle;
            else if (c == QLatin1Char('>'))
                angle = qMax(0, angle - 1);
            else if (c == QLatin1Char('(') && angle == 0) {
                paren = k;
                break;
            }
        }
        if (paren < 0)
            return p;
        int e = paren;
        while (e > 0 && s[e - 1] == QLatin1Char(' '))
            --e;
        int b = e;
        while (b > 0 && isIdent(s[b - 1]))
            --b;
        if (b > 0 && s[b - 1] == QLatin1Char('~'))
            --b;
        nameStart = b;
        p.name = s.mid(b, e - b);
        if (p.name.isEmpty() || p.name[0].isDigit())
            return p;
    }
    const int close = matching(s, paren);
    if (close < 0)
        return p;
    p.prefix = s.left(nameStart).trimmed();
    p.params = s.mid(paren + 1, close - paren - 1);
    p.trailing = s.mid(close + 1).trimmed();
    if (p.prefix.endsWith(QLatin1String("::")))
        return p; // a redeclaration of something defined elsewhere
    const QString bare = className.left(className.indexOf(QLatin1Char('<')) < 0 ? className.size() : className.indexOf(QLatin1Char('<')));
    const bool special = (!bare.isEmpty() && (p.name == bare || p.name == QLatin1Char('~') + bare)) ||
                         p.name.startsWith(QLatin1String("operator "));
    if (p.prefix.isEmpty() && !special)
        return p; // a call or a macro, not a declaration
    static const QRegularExpression removed(QStringLiteral("=\\s*(0|default|delete)$"));
    const QRegularExpressionMatch rm = removed.match(p.trailing);
    if (rm.hasMatch()) {
        p.error = rm.captured(1) == QLatin1String("0") ? tr("\"%1\" is pure virtual").arg(p.name)
                                                       : tr("\"%1\" is %2d").arg(p.name, rm.captured(1));
        return p;
    }
    const int arrow = p.trailing.indexOf(QLatin1String("->"));
    const QString qualifiers = arrow < 0 ? p.trailing : p.trailing.left(arrow);
    if (qualifiers.contains(QLatin1Char('=')) ||
        (qualifiers.contains(QLatin1Char('(')) && !qualifiers.contains(QLatin1String("noexcept")) && !qualifiers.contains(QLatin1String("throw"))))
        return p; // `int x = f(1);` or a function pointer
    p.ok = true;
    p.error.clear();
    return p;
}

// Walks the statements of the block [from, to) (the whole file, a namespace or a class body) to the one holding `pos`. The
// scopes on the way go to `chain`; `siblings` gets the statements of the innermost block and `index` ours among them.
bool walk(const QString &b, int from, int to, int pos, QVector<Scope> &chain, QVector<Statement> &siblings, int *index, QString *error)
{
    static const QRegularExpression access(QStringLiteral("(?:^|\\s)(public|protected|private|signals|Q_SIGNALS|slots|Q_SLOTS)(?:\\s+(?:slots|Q_SLOTS))?$"));
    int start = from;
    int depth = 0;
    bool signalSection = false;
    for (int i = from; i < to; ++i) {
        const QChar c = b[i];
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) {
            ++depth;
        } else if (c == QLatin1Char(')') || c == QLatin1Char(']')) {
            depth = qMax(0, depth - 1);
        } else if (c == QLatin1Char('{')) {
            int close = matching(b, i, to);
            if (close < 0)
                close = to;
            if (depth > 0) { // a braced initializer inside a parameter list
                i = close;
                continue;
            }
            const QString head = b.mid(start, i - start).simplified();
            if (pos > i && pos <= close) {
                Scope scope;
                scope.open = i;
                scope.close = close;
                const QRegularExpressionMatch ns = namespaceHead().match(head);
                if (ns.hasMatch()) {
                    for (const QString &part : ns.captured(1).split(QStringLiteral("::"), Qt::SkipEmptyParts))
                        scope.names << part.trimmed().remove(QStringLiteral("inline")).trimmed();
                    if (scope.names.isEmpty())
                        scope.names << QString(); // anonymous
                    chain << scope;
                } else if (classHead(head, &scope)) {
                    chain << scope;
                } else if (!isExternBlock(head)) {
                    *error = tr("Put the caret on a function declaration (this is inside a body)");
                    return false;
                }
                siblings.clear();
                return walk(b, i + 1, close, pos, chain, siblings, index, error);
            }
            if (pos >= start && pos <= i) {
                *error = head.contains(QLatin1Char('(')) ? tr("This function already has a body")
                                                          : tr("Put the caret on a function declaration");
                return false;
            }
            int k = close + 1;
            while (k < to && b[k].isSpace())
                ++k;
            i = k < to && b[k] == QLatin1Char(';') ? k : close;
            start = i + 1;
        } else if (c == QLatin1Char(';') && depth == 0) {
            siblings.append({start, i, signalSection});
            if (pos >= start && pos <= i + 1) {
                *index = siblings.size() - 1;
                return true;
            }
            start = i + 1;
        } else if (c == QLatin1Char(':') && depth == 0 && (i + 1 >= to || b[i + 1] != QLatin1Char(':')) && (i == 0 || b[i - 1] != QLatin1Char(':'))) {
            const QRegularExpressionMatch m = access.match(b.mid(start, i - start).simplified());
            if (m.hasMatch()) {
                signalSection = m.captured(1) == QLatin1String("signals") || m.captured(1) == QLatin1String("Q_SIGNALS");
                start = i + 1;
            }
        }
    }
    *error = tr("Put the caret on a function declaration");
    return false;
}

QString classBase(const QString &cls)
{
    const int lt = cls.indexOf(QLatin1Char('<'));
    return lt < 0 ? cls : cls.left(lt);
}

// Names of the types declared inside a class body (nested classes, enums, aliases): a return type naming one of them needs
// the class in front of it outside the class.
QSet<QString> nestedTypes(const QString &body)
{
    QSet<QString> out;
    static const QRegularExpression decl(QStringLiteral("\\b(?:class|struct|union|enum(?:\\s+class|\\s+struct)?)\\s+(\\w+)"));
    static const QRegularExpression alias(QStringLiteral("\\busing\\s+(\\w+)\\s*="));
    static const QRegularExpression typedefs(QStringLiteral("\\btypedef\\b[^;{}]*?(\\w+)\\s*;"));
    for (const QRegularExpression *re : {&decl, &alias, &typedefs})
        for (auto it = re->globalMatch(body); it.hasNext();)
            out.insert(it.next().captured(1));
    return out;
}

// Puts `qualifier` (with the namespace marker \x01 in front) before the first nested type name in `part`.
QString qualifyNested(const QString &part, const QSet<QString> &nested, const QString &qualifier, bool typenameNeeded)
{
    static const QSet<QString> skip{QStringLiteral("const"), QStringLiteral("constexpr"), QStringLiteral("consteval"),
                                    QStringLiteral("inline"), QStringLiteral("volatile"), QStringLiteral("unsigned"),
                                    QStringLiteral("signed"), QStringLiteral("typename"), QStringLiteral("auto")};
    static const QRegularExpression word(QStringLiteral("\\b\\w+\\b"));
    for (auto it = word.globalMatch(part); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (skip.contains(m.captured()))
            continue;
        if (!nested.contains(m.captured()) || part.mid(0, m.capturedStart()).trimmed().endsWith(QLatin1String("::")))
            return part;
        return part.left(m.capturedStart()) + (typenameNeeded ? QStringLiteral("typename ") : QString()) + QLatin1Char('\x01') + qualifier +
               QStringLiteral("::") + part.mid(m.capturedStart());
    }
    return part;
}

// The namespaces open at `pos`, outermost first ("" for an anonymous one), and the blocks that close them.
struct NamespaceBlock {
    QStringList chain;
    int close = -1;
};
QStringList openNamespaces(const QString &b, int pos, QVector<NamespaceBlock> *blocks = nullptr)
{
    struct Open {
        QStringList names; // empty: not a namespace (a class, a function, extern "C" counts as transparent)
        bool ns = false;
        bool transparent = false;
    };
    QVector<Open> stack;
    QStringList atPos;
    bool captured = false;
    int boundary = 0;
    auto chainNow = [&] {
        QStringList chain;
        for (const Open &o : std::as_const(stack)) {
            if (o.ns)
                chain += o.names;
            else if (!o.transparent)
                return QStringList{QStringLiteral("\x02")}; // inside something that is not a namespace
        }
        return chain;
    };
    for (int i = 0; i < b.size(); ++i) {
        if (i == pos && !captured) {
            atPos = chainNow();
            captured = true;
            if (!blocks)
                return atPos;
        }
        const QChar c = b[i];
        if (c == QLatin1Char('{')) {
            const QString head = b.mid(boundary, i - boundary).simplified();
            Open o;
            const QRegularExpressionMatch ns = namespaceHead().match(head);
            if (ns.hasMatch()) {
                o.ns = true;
                for (const QString &part : ns.captured(1).split(QStringLiteral("::"), Qt::SkipEmptyParts))
                    o.names << part.trimmed().remove(QStringLiteral("inline")).trimmed();
                if (o.names.isEmpty())
                    o.names << QString();
            } else {
                o.transparent = isExternBlock(head);
            }
            stack << o;
            boundary = i + 1;
        } else if (c == QLatin1Char('}')) {
            if (!stack.isEmpty()) {
                if (blocks && stack.last().ns) {
                    const QStringList chain = chainNow();
                    blocks->append({chain, i});
                }
                stack.removeLast();
            }
            boundary = i + 1;
        } else if (c == QLatin1Char(';')) {
            boundary = i + 1;
        }
    }
    return captured ? atPos : chainNow();
}

// The definition of `name` (a member of the declaration's innermost class, or a free function) in the blanked text: the
// position of the name, -1 when there is none. `types` (when given) must match its parameters; `bodyEnd` gets the closing brace.
int findDefinition(const QString &b, const Declaration &d, const QString &name, const QStringList *types, int *bodyEnd)
{
    QString namePattern;
    if (name.startsWith(QLatin1String("operator"))) {
        namePattern = QStringLiteral("\\boperator\\s*") + QRegularExpression::escape(name.mid(8).trimmed());
    } else if (name.startsWith(QLatin1Char('~'))) {
        namePattern = QStringLiteral("~\\s*") + QRegularExpression::escape(name.mid(1));
    } else {
        namePattern = QStringLiteral("\\b") + QRegularExpression::escape(name);
    }
    QString pattern;
    if (!d.classes.isEmpty())
        pattern = QStringLiteral("\\b") + QRegularExpression::escape(classBase(d.classes.last())) +
                  QStringLiteral("(?:\\s*<[^;{}()]*>)?\\s*::\\s*") + namePattern + QStringLiteral("\\s*\\(");
    else
        pattern = QStringLiteral("(?<![\\w.>~])(?:\\w+\\s*::\\s*)*") + namePattern + QStringLiteral("\\s*\\(");
    const QRegularExpression re(pattern);
    for (auto it = re.globalMatch(b); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int paren = m.capturedEnd() - 1;
        const int close = matching(b, paren);
        if (close < 0)
            continue;
        // After the parameters: qualifiers, then a body (`{`, or `:` and a constructor's initialisers first).
        int k = close + 1;
        int body = -1;
        while (k < b.size()) {
            const QChar c = b[k];
            if (c.isSpace() || isIdent(c) || c == QLatin1Char('&') || c == QLatin1Char('*') || c == QLatin1Char('<') ||
                c == QLatin1Char('>') || c == QLatin1Char('-') || (c == QLatin1Char(':') && k + 1 < b.size() && b[k + 1] == QLatin1Char(':'))) {
                k += (c == QLatin1Char(':')) ? 2 : 1;
            } else if (c == QLatin1Char('(')) { // noexcept(...), throw(...)
                const int e = matching(b, k);
                if (e < 0)
                    break;
                k = e + 1;
            } else if (c == QLatin1Char('{')) {
                body = k;
                break;
            } else if (c == QLatin1Char(':')) { // member initialisers: name(...) or name{...}, separated by commas
                ++k;
                while (k < b.size()) {
                    while (k < b.size() && b[k] != QLatin1Char('(') && b[k] != QLatin1Char('{') && b[k] != QLatin1Char(';'))
                        ++k;
                    if (k >= b.size() || b[k] == QLatin1Char(';'))
                        break;
                    const int e = matching(b, k);
                    if (e < 0)
                        break;
                    k = e + 1;
                    while (k < b.size() && b[k].isSpace())
                        ++k;
                    if (k < b.size() && b[k] == QLatin1Char(',')) {
                        ++k;
                        continue;
                    }
                    if (k < b.size() && b[k] == QLatin1Char('{'))
                        body = k;
                    break;
                }
                break;
            } else {
                break;
            }
        }
        if (body < 0)
            continue;
        if (types && typeKeys(b.mid(paren + 1, close - paren - 1)) != *types)
            continue;
        if (bodyEnd)
            *bodyEnd = matching(b, body);
        return m.capturedStart();
    }
    return -1;
}

int lineStartAt(const QString &text, int pos)
{
    const int nl = text.lastIndexOf(QLatin1Char('\n'), qMax(0, pos - 1));
    return pos <= 0 ? 0 : nl + 1;
}

int lineAfter(const QString &text, int pos)
{
    const int nl = text.indexOf(QLatin1Char('\n'), pos);
    return nl < 0 ? text.size() : nl + 1;
}

// Where the definition whose name is at `namePos` starts (its template line or return type), moved above the comment lines
// directly over it.
int definitionStart(const QString &text, const QString &b, int namePos)
{
    int k = namePos;
    while (k > 0 && b[k - 1] != QLatin1Char(';') && b[k - 1] != QLatin1Char('}') && b[k - 1] != QLatin1Char('{'))
        --k;
    while (k < namePos && b[k].isSpace())
        ++k;
    int line = lineStartAt(text, k);
    while (line > 0) {
        const int prev = lineStartAt(text, line - 1);
        const QString t = text.mid(prev, line - prev).trimmed();
        if (!(t.startsWith(QLatin1String("//")) || t.startsWith(QLatin1String("/*")) || t.startsWith(QLatin1Char('*'))))
            break;
        line = prev;
    }
    return line;
}

} // namespace

bool isHeader(const QString &path)
{
    return headerExts().contains(suffixOf(path));
}

bool isSource(const QString &path)
{
    return sourceExts().contains(suffixOf(path));
}

QStringList counterparts(const QString &path, const QStringList &projectFiles)
{
    const bool header = isHeader(path);
    if (!header && !isSource(path))
        return {};
    const QStringList exts = header ? sourceExts() : headerExts().mid(0, 5);
    const QFileInfo fi(path);
    const QString base = fi.completeBaseName();
    QStringList out;
    for (const QString &ext : exts) {
        const QString candidate = fi.absoluteDir().filePath(base + QLatin1Char('.') + ext);
        if (QFileInfo::exists(candidate))
            out << candidate;
    }
    // Elsewhere in the project (include/ beside src/ ...): the closest folder first.
    const QString dir = fi.absolutePath() + QLatin1Char('/');
    QVector<QPair<int, QString>> found;
    for (const QString &f : projectFiles) {
        if (baseOf(f) != base || !exts.contains(suffixOf(f)) || out.contains(f))
            continue;
        int shared = 0;
        while (shared < dir.size() && shared < f.size() && dir[shared] == f[shared])
            ++shared;
        found.append({-shared, f});
    }
    std::sort(found.begin(), found.end());
    for (const auto &f : std::as_const(found))
        out << f.second;
    return out;
}

QString newFileText(const QString &path, const QString &projectRoot)
{
    const QFileInfo fi(path);
    if (isSource(path)) {
        for (const QString &ext : headerExts().mid(0, 5)) {
            const QString header = fi.completeBaseName() + QLatin1Char('.') + ext;
            if (fi.absoluteDir().exists(header))
                return QStringLiteral("#include \"%1\"\n\n").arg(header);
        }
        return {};
    }
    if (!headerExts().mid(0, 5).contains(fi.suffix().toLower()))
        return {};
    // Learn from the project's headers: #pragma once, or guards (and how they are named).
    static const QRegularExpression guardRe(QStringLiteral("^\\s*#\\s*ifndef\\s+(\\w+)\\s*\\n\\s*#\\s*define\\s+(\\w+)"), QRegularExpression::MultilineOption);
    int pragmas = 0, guards = 0;
    QString sampleGuard, sampleFile;
    for (const QString &h : sampleHeaders(fi.absolutePath(), projectRoot, 12)) {
        QFile f(h);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QString head = QString::fromUtf8(f.read(4096));
        if (head.contains(QLatin1String("#pragma once"))) {
            ++pragmas;
            continue;
        }
        const QRegularExpressionMatch m = guardRe.match(head);
        if (m.hasMatch() && m.captured(1) == m.captured(2)) {
            ++guards;
            if (sampleGuard.isEmpty()) {
                sampleGuard = m.captured(1);
                sampleFile = h;
            }
        }
    }
    if (guards <= pragmas)
        return QStringLiteral("#pragma once\n\n");
    // Name it like the sample: whatever stood around that file's base name (a project prefix, _H / _HPP / a trailing _).
    const QString ours = guardWord(fi.completeBaseName());
    QString guard = ours + QLatin1Char('_') + guardWord(fi.suffix());
    const QString theirs = guardWord(QFileInfo(sampleFile).completeBaseName());
    const int at = sampleGuard.indexOf(theirs);
    if (at >= 0) {
        QString tail = sampleGuard.mid(at + theirs.size());
        const QString theirExt = guardWord(QFileInfo(sampleFile).suffix());
        tail.replace(theirExt, guardWord(fi.suffix()));
        guard = sampleGuard.left(at) + ours + tail;
    }
    return QStringLiteral("#ifndef %1\n#define %1\n\n\n\n#endif // %1\n").arg(guard);
}

bool includeContext(const QString &lineBeforeCaret, QChar *opener, QString *typed)
{
    static const QRegularExpression re(QStringLiteral("^\\s*#\\s*(?:include|include_next|import)\\s*([<\"])([^<>\"]*)$"));
    const QRegularExpressionMatch m = re.match(lineBeforeCaret);
    if (!m.hasMatch())
        return false;
    if (opener)
        *opener = m.captured(1).at(0);
    if (typed)
        *typed = m.captured(2);
    return true;
}

bool atIncludeOpener(const QString &lineBeforeCaret)
{
    static const QRegularExpression re(QStringLiteral("^\\s*#\\s*(?:include|include_next|import)\\s*$"));
    return re.match(lineBeforeCaret).hasMatch();
}

QStringList systemIncludeDirs()
{
    static const QStringList dirs = [] {
        QStringList out;
        // The newest numbered folder below `parent` ("/usr/include/c++/13"), with `suffix` appended.
        auto newest = [](const QString &parent) {
            int best = -1;
            QString found;
            for (const QString &v : QDir(parent).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const int n = v.section(QLatin1Char('.'), 0, 0).toInt();
                if (n > best) {
                    best = n;
                    found = parent + QLatin1Char('/') + v;
                }
            }
            return found;
        };
        const QString cxx = newest(QStringLiteral("/usr/include/c++"));
        if (!cxx.isEmpty())
            out << cxx;
        for (const QString &arch : QDir(QStringLiteral("/usr/include")).entryList({QStringLiteral("*-linux-gnu*")}, QDir::Dirs)) {
            const QString archDir = QStringLiteral("/usr/include/") + arch;
            const QString archCxx = newest(archDir + QStringLiteral("/c++"));
            if (!archCxx.isEmpty())
                out << archCxx;
            out << archDir;
        }
        out << QStringLiteral("/usr/local/include") << QStringLiteral("/usr/include");
        QStringList existing;
        for (const QString &d : std::as_const(out))
            if (QFileInfo(d).isDir())
                existing << d;
        return existing;
    }();
    return dirs;
}

QVector<PathEntry> includeEntries(const QStringList &folders, const QString &subfolder)
{
    QVector<PathEntry> out;
    QSet<QString> seen;
    for (const QString &folder : folders) {
        const QDir dir(subfolder.isEmpty() ? folder : folder + QLatin1Char('/') + subfolder);
        if (!dir.exists())
            continue;
        const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &fi : entries) {
            const QString name = fi.fileName();
            if (name.startsWith(QLatin1Char('.')) || seen.contains(name))
                continue;
            const bool isDir = fi.isDir();
            if (!isDir && !fi.suffix().isEmpty() && !headerExts().contains(fi.suffix().toLower()))
                continue;
            seen.insert(name);
            out.append({name, isDir});
            if (out.size() >= 3000)
                return out;
        }
    }
    return out;
}

Declaration declarationAt(const QString &text, int position)
{
    Declaration d;
    const QString b = blanked(text);
    QVector<Scope> chain;
    QVector<Statement> siblings;
    int index = -1;
    if (!walk(b, 0, b.size(), position, chain, siblings, &index, &d.error))
        return d;
    QString className;
    for (const Scope &s : std::as_const(chain))
        if (s.isClass)
            className = s.names.value(0);
    const Statement &st = siblings.at(index);
    const Parsed p = parseStatement(b.mid(st.start, st.end - st.start).simplified(), className);
    if (!p.ok) {
        d.error = p.error;
        return d;
    }
    if (st.signal) {
        d.error = tr("\"%1\" is a signal: moc writes its definition").arg(p.name);
        return d;
    }

    // Scopes: namespaces outside the classes, the classes (nested ones in order) with their template lines.
    QStringList templateLines;
    int outerClass = -1, innerClass = -1, innerNamespace = -1;
    for (int i = 0; i < chain.size(); ++i) {
        const Scope &s = chain.at(i);
        if (s.isClass) {
            if (outerClass < 0)
                outerClass = i;
            innerClass = i;
            d.classes << s.names.value(0);
            if (!s.templateLine.isEmpty())
                templateLines << s.templateLine;
        } else if (outerClass < 0) {
            innerNamespace = i;
            for (const QString &n : s.names) {
                if (n.isEmpty())
                    d.keepInFile = true; // anonymous namespace: only this file can define it
                else
                    d.namespaces << n;
            }
        }
    }
    if (!p.memberTemplate.isEmpty())
        templateLines << p.memberTemplate;

    // The return type loses what only belongs to the declaration; constexpr / inline keep the definition in the header.
    static const QRegularExpression dropped(QStringLiteral("\\b(?:virtual|static|explicit|extern|Q_INVOKABLE|Q_SLOT|Q_SIGNAL)\\b\\s*"));
    static const QRegularExpression kept(QStringLiteral("\\b(?:inline|constexpr|consteval)\\b"));
    QString prefix = p.prefix;
    if (d.classes.isEmpty()) // a free function: `static` is its linkage, which the definition may repeat
        prefix.remove(QRegularExpression(QStringLiteral("\\b(?:extern)\\b\\s*")));
    else
        prefix.remove(dropped);
    prefix = prefix.simplified();
    QString trailing = p.trailing;
    trailing.remove(QRegularExpression(QStringLiteral("\\b(?:override|final|Q_DECL_OVERRIDE|Q_DECL_FINAL)\\b")));
    trailing = trailing.simplified();
    if (kept.match(prefix).hasMatch() || !templateLines.isEmpty())
        d.keepInFile = true;

    if (innerClass >= 0) {
        const Scope &cls = chain.at(innerClass);
        const QSet<QString> nested = nestedTypes(b.mid(cls.open + 1, cls.close - cls.open - 1));
        const QString qualifier = d.classes.join(QStringLiteral("::"));
        const bool templated = !templateLines.isEmpty();
        prefix = qualifyNested(prefix, nested, qualifier, templated);
        const int arrow = trailing.indexOf(QLatin1String("->"));
        if (arrow >= 0)
            trailing = trailing.left(arrow + 2) + QLatin1Char(' ') + qualifyNested(trailing.mid(arrow + 2).trimmed(), nested, qualifier, templated);
    }

    d.name = p.name;
    d.head = templateLines.isEmpty() ? QString() : templateLines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    if (!prefix.isEmpty())
        d.head += prefix + QLatin1Char(' ');
    d.tail = QLatin1Char('(') + joinParams(p.params) + QLatin1Char(')') + (trailing.isEmpty() ? QString() : QLatin1Char(' ') + trailing);
    d.paramTypes = typeKeys(p.params);

    // Where a definition kept in this file goes: after the outermost class, or the end of the namespace / file.
    if (outerClass >= 0) {
        int k = chain.at(outerClass).close + 1;
        while (k < b.size() && (b[k] == QLatin1Char(' ') || b[k] == QLatin1Char('\t')))
            ++k;
        if (k < b.size() && b[k] == QLatin1Char(';'))
            ++k;
        d.classEnd = lineAfter(text, k);
    }
    if (innerNamespace >= 0)
        d.scopeEnd = chain.at(innerNamespace).close;

    // The functions declared around it in the same scope: their definitions show where this one belongs.
    for (int i = 0; i < siblings.size(); ++i) {
        if (i == index)
            continue;
        const Parsed other = parseStatement(b.mid(siblings[i].start, siblings[i].end - siblings[i].start).simplified(), className);
        if (!other.ok || other.name == d.name)
            continue;
        if (i < index)
            d.before.prepend(other.name);
        else
            d.after << other.name;
    }
    d.ok = true;
    return d;
}

Insertion insertionFor(const Declaration &d, const QString &target, bool sameFile, const QString &indent)
{
    Insertion ins;
    const QString b = blanked(target);
    const int existing = findDefinition(b, d, d.name, &d.paramTypes, nullptr);
    if (existing >= 0) {
        ins.exists = true;
        ins.position = existing;
        return ins;
    }
    int pos = -1;
    for (const QString &name : d.before) {
        int end = -1;
        if (findDefinition(b, d, name, nullptr, &end) >= 0 && end >= 0) {
            pos = lineAfter(target, end);
            break;
        }
    }
    if (pos < 0) {
        for (const QString &name : d.after) {
            const int at = findDefinition(b, d, name, nullptr, nullptr);
            if (at >= 0) {
                pos = definitionStart(target, b, at);
                break;
            }
        }
    }
    if (pos < 0) {
        if (sameFile && d.classEnd >= 0) {
            pos = d.classEnd;
        } else if (sameFile && d.scopeEnd >= 0) {
            pos = lineStartAt(target, d.scopeEnd);
        } else if (!sameFile && !d.namespaces.isEmpty()) {
            QVector<NamespaceBlock> blocks;
            openNamespaces(b, b.size(), &blocks);
            for (const NamespaceBlock &block : std::as_const(blocks))
                if (block.chain == d.namespaces)
                    pos = lineStartAt(target, block.close);
        }
    }
    if (pos < 0)
        pos = target.size();

    // Namespaces already open at that point are left out of the name.
    QStringList open = openNamespaces(b, pos);
    QStringList rest = d.namespaces;
    while (!open.isEmpty() && !rest.isEmpty() && open.first() == rest.first()) {
        open.removeFirst();
        rest.removeFirst();
    }
    QString nsPrefix;
    for (const QString &n : std::as_const(rest))
        nsPrefix += n + QStringLiteral("::");
    QString head = d.head;
    head.replace(QLatin1Char('\x01'), nsPrefix);
    QString qualified = nsPrefix;
    for (const QString &c : d.classes)
        qualified += c + QStringLiteral("::");
    qualified += d.name;
    QString tail = d.tail;
    tail.replace(QLatin1Char('\x01'), nsPrefix);
    const QString def = head + qualified + tail + QStringLiteral("\n{\n") + indent + QStringLiteral("\n}");

    // A blank line on either side.
    QString before = target.left(pos);
    QString pre;
    if (!before.isEmpty() && !before.endsWith(QLatin1Char('\n')))
        pre = QStringLiteral("\n");
    static const QRegularExpression blankEnd(QStringLiteral("\\n[ \\t]*\\n$"));
    before += pre;
    if (!before.isEmpty() && !blankEnd.match(before).hasMatch() && before != QLatin1String("\n"))
        pre += QLatin1Char('\n');
    const int nextEnd = target.indexOf(QLatin1Char('\n'), pos);
    const bool nextBlank = pos >= target.size() || target.mid(pos, (nextEnd < 0 ? target.size() : nextEnd) - pos).trimmed().isEmpty();
    ins.position = pos;
    ins.text = pre + def + QStringLiteral("\n") + (nextBlank ? QString() : QStringLiteral("\n"));
    ins.caret = pre.size() + def.indexOf(QLatin1String("{\n")) + 2 + indent.size();
    return ins;
}

} // namespace CppTools
