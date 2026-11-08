#include "ktor/KtorSource.h"
#include "editor/TextEncoding.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

QString KtorRoute::kindName() const
{
    switch (kind) {
    case WebSocket: return QStringLiteral("WebSocket");
    case Sse: return QStringLiteral("Server-sent events");
    case Static: return QStringLiteral("Static content");
    case Docs: return QStringLiteral("API docs");
    case Http: break;
    }
    return QStringLiteral("HTTP");
}

namespace {

struct Literal
{
    int start = 0, end = 0; // the opening quote, one past the closing one
    QString value;          // the text between the quotes, as written
};

// One call of the DSL: name<Generic>(args) { lambda }.
struct Site
{
    QString name;
    QString generic;            // get<Articles.Id> -> "Articles.Id"
    int at = 0;                 // offset of the name
    int argsOpen = -1, argsClose = -1;
    int lambdaOpen = -1, lambdaClose = -1;
    bool hasLambda() const { return lambdaOpen >= 0; }
    bool contains(int pos) const { return hasLambda() && pos > lambdaOpen && pos < lambdaClose; }
};

struct Function
{
    QString receiver, name;
    int at = 0, bodyStart = 0, bodyEnd = 0, line = 1;
};

struct ResourceClass
{
    QString name, qualified, ownPath, path;
    int at = 0, bodyOpen = -1, bodyClose = -1;
    QVector<SpringParam> props; // constructor properties (without `parent`)
};

struct FileData
{
    QString path, text, noComments, masked, pkg, jvmName;
    QVector<int> lineStarts;
    QVector<Literal> literals;
    QVector<Site> sites;
    QVector<Function> functions;
    QVector<ResourceClass> resources;

    int lineAt(int off) const { return int(std::upper_bound(lineStarts.begin(), lineStarts.end(), off) - lineStarts.begin()); }
    const Literal *literalAt(int quote) const
    {
        auto it = std::lower_bound(literals.begin(), literals.end(), quote, [](const Literal &l, int q) { return l.start < q; });
        return it != literals.end() && it->start == quote ? &*it : nullptr;
    }
    const Function *functionAt(int pos) const
    {
        const Function *best = nullptr;
        for (const Function &f : functions)
            if (pos > f.bodyStart && pos < f.bodyEnd && (!best || f.bodyStart > best->bodyStart))
                best = &f;
        return best;
    }
};

const QSet<QString> &verbs()
{
    static const QSet<QString> v{QStringLiteral("get"), QStringLiteral("post"), QStringLiteral("put"), QStringLiteral("delete"),
                                 QStringLiteral("patch"), QStringLiteral("head"), QStringLiteral("options")};
    return v;
}

// Comments become blanks in `noComments` and `masked`; string contents become blanks in `masked` only. Offsets and
// line breaks stay, so positions found in `masked` index the original text.
void mask(FileData &f)
{
    const QString &s = f.text;
    const int n = s.size();
    f.noComments = s;
    f.masked = s;
    auto blank = [](QString &t, int from, int to) {
        for (int k = from; k < to; ++k)
            if (t.at(k) != QLatin1Char('\n'))
                t[k] = QLatin1Char(' ');
    };
    int i = 0;
    while (i < n) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('/')) {
            int e = s.indexOf(QLatin1Char('\n'), i);
            e = e < 0 ? n : e;
            blank(f.noComments, i, e);
            blank(f.masked, i, e);
            i = e;
        } else if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('*')) {
            int e = s.indexOf(QLatin1String("*/"), i + 2);
            e = e < 0 ? n : e + 2;
            blank(f.noComments, i, e);
            blank(f.masked, i, e);
            i = e;
        } else if (c == QLatin1Char('"')) {
            const bool raw = s.mid(i, 3) == QLatin1String("\"\"\"");
            int j = i + (raw ? 3 : 1);
            int templ = 0; // depth inside ${ … }
            while (j < n) {
                const QChar d = s.at(j);
                if (templ > 0) {
                    if (d == QLatin1Char('{'))
                        ++templ;
                    else if (d == QLatin1Char('}'))
                        --templ;
                    ++j;
                    continue;
                }
                if (d == QLatin1Char('$') && j + 1 < n && s.at(j + 1) == QLatin1Char('{')) {
                    templ = 1;
                    j += 2;
                    continue;
                }
                if (raw) {
                    if (s.mid(j, 3) == QLatin1String("\"\"\""))
                        break;
                } else {
                    if (d == QLatin1Char('\\')) {
                        j += 2;
                        continue;
                    }
                    if (d == QLatin1Char('"') || d == QLatin1Char('\n'))
                        break;
                }
                ++j;
            }
            const int contentStart = i + (raw ? 3 : 1);
            const int contentEnd = qMin(j, n);
            const int end = qMin(n, contentEnd + (raw ? 3 : 1));
            f.literals.append({i, end, s.mid(contentStart, contentEnd - contentStart)});
            blank(f.masked, contentStart, contentEnd);
            i = end;
        } else if (c == QLatin1Char('\'')) {
            // A char literal: 'x', '\n', 'A'.
            int e = s.indexOf(QLatin1Char('\''), i + 1 + (i + 1 < n && s.at(i + 1) == QLatin1Char('\\') ? 2 : 0));
            if (e < 0 || e - i > 8)
                e = i;
            blank(f.masked, i + 1, e);
            i = e + 1;
        } else {
            ++i;
        }
    }
}

// The bracket closing the one at `open` in masked text (strings and comments are blanks there), or -1.
int closeOf(const QString &t, int open)
{
    const QChar o = t.at(open);
    const QChar c = o == QLatin1Char('(') ? QLatin1Char(')') : o == QLatin1Char('{') ? QLatin1Char('}') : o == QLatin1Char('<') ? QLatin1Char('>') : QLatin1Char(']');
    int depth = 0;
    for (int i = open; i < t.size(); ++i) {
        const QChar ch = t.at(i);
        if (ch == o)
            ++depth;
        else if (ch == c && --depth == 0)
            return i;
        else if (o == QLatin1Char('<') && (ch == QLatin1Char('{') || ch == QLatin1Char(';') || ch == QLatin1Char('\n')))
            return -1; // a comparison, not a type argument list
    }
    return -1;
}

int skipSpace(const QString &t, int i)
{
    while (i < t.size() && t.at(i).isSpace())
        ++i;
    return i;
}

// Top-level comma-separated pieces of [from, to) in masked text: offsets of each piece.
QVector<QPair<int, int>> splitArgs(const QString &t, int from, int to)
{
    QVector<QPair<int, int>> out;
    int depth = 0, start = from;
    for (int i = from; i < to; ++i) {
        const QChar c = t.at(i);
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'))
            --depth;
        else if (c == QLatin1Char(',') && depth == 0) {
            out.append({start, i});
            start = i + 1;
        }
    }
    if (to > start && !t.mid(start, to - start).trimmed().isEmpty())
        out.append({start, to});
    return out;
}

QString joinPath(const QString &a, const QString &b)
{
    QString out = a;
    if (!b.isEmpty()) {
        if (!out.endsWith(QLatin1Char('/')))
            out += QLatin1Char('/');
        out += b.startsWith(QLatin1Char('/')) ? b.mid(1) : b;
    }
    if (!out.startsWith(QLatin1Char('/')))
        out.prepend(QLatin1Char('/'));
    while (out.size() > 1 && out.endsWith(QLatin1Char('/')))
        out.chop(1);
    out.replace(QStringLiteral("//"), QStringLiteral("/"));
    return out;
}

void findSites(FileData &f)
{
    static const QRegularExpression name(QStringLiteral(
        R"(\b(route|get|post|put|delete|patch|head|options|webSocket|webSocketRaw|sse|authenticate|method|handle|routing|install|authentication|staticResources|staticFiles|staticFileSystem|staticZip|singlePageApplication|swaggerUI|openAPI)\b)"));
    const QString &t = f.masked;
    for (QRegularExpressionMatchIterator it = name.globalMatch(t); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        // Not a member call (map.get(…), call.request.headers.get) and not a declaration (fun get(…)).
        int b = m.capturedStart() - 1;
        while (b >= 0 && (t.at(b) == QLatin1Char(' ') || t.at(b) == QLatin1Char('\t')))
            --b;
        if (b >= 0 && (t.at(b) == QLatin1Char('.') || t.at(b) == QLatin1Char(':')))
            continue;
        if (b >= 2 && t.mid(b - 2, 3) == QLatin1String("fun"))
            continue;
        Site s;
        s.name = m.captured(1);
        s.at = m.capturedStart();
        int i = skipSpace(t, m.capturedEnd());
        if (i < t.size() && t.at(i) == QLatin1Char('<')) {
            const int c = closeOf(t, i);
            if (c < 0)
                continue;
            s.generic = t.mid(i + 1, c - i - 1).simplified().remove(QLatin1Char(' '));
            i = skipSpace(t, c + 1);
        }
        if (i < t.size() && t.at(i) == QLatin1Char('(')) {
            const int c = closeOf(t, i);
            if (c < 0)
                continue;
            s.argsOpen = i;
            s.argsClose = c;
            i = skipSpace(t, c + 1);
        }
        if (i < t.size() && t.at(i) == QLatin1Char('{')) {
            const int c = closeOf(t, i);
            if (c > 0) {
                s.lambdaOpen = i;
                s.lambdaClose = c;
            }
        }
        if (s.argsOpen < 0 && !s.hasLambda())
            continue; // a plain identifier
        f.sites.append(s);
    }
}

void findFunctions(FileData &f)
{
    static const QRegularExpression fun(QStringLiteral(R"(\bfun\s+(?:<[^>{}()]*>\s*)?(?:([\w.]+(?:<[^>{}()]*>)?)\.)?(\w+)\s*\()"));
    const QString &t = f.masked;
    for (QRegularExpressionMatchIterator it = fun.globalMatch(t); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        Function fn;
        fn.receiver = m.captured(1);
        fn.receiver = fn.receiver.left(fn.receiver.indexOf(QLatin1Char('<')) < 0 ? fn.receiver.size() : fn.receiver.indexOf(QLatin1Char('<')));
        fn.receiver = fn.receiver.mid(fn.receiver.lastIndexOf(QLatin1Char('.')) + 1);
        fn.name = m.captured(2);
        fn.at = m.capturedStart(2);
        fn.line = f.lineAt(fn.at);
        const int close = closeOf(t, m.capturedEnd() - 1);
        if (close < 0)
            continue;
        // `: ReturnType` then `{` (block body) or `=` (expression body).
        int i = close + 1;
        while (i < t.size() && t.at(i) != QLatin1Char('{') && t.at(i) != QLatin1Char('=') && t.at(i) != QLatin1Char('\n'))
            ++i;
        if (i < t.size() && t.at(i) == QLatin1Char('\n')) {
            const int next = skipSpace(t, i);
            if (next < t.size() && (t.at(next) == QLatin1Char('{') || t.at(next) == QLatin1Char('=')))
                i = next;
        }
        if (i >= t.size())
            continue;
        if (t.at(i) == QLatin1Char('{')) {
            fn.bodyStart = i;
            fn.bodyEnd = closeOf(t, i);
        } else if (t.at(i) == QLatin1Char('=')) {
            // An expression body: up to the end of its first block (`= route("/x") { … }`), else the end of the line.
            fn.bodyStart = i;
            const int nl = t.indexOf(QLatin1Char('\n'), i);
            const int brace = t.indexOf(QLatin1Char('{'), i);
            fn.bodyEnd = brace >= 0 && (nl < 0 || brace < nl) ? closeOf(t, brace) : (nl < 0 ? int(t.size()) : nl);
        } else {
            continue; // abstract / interface function
        }
        if (fn.bodyEnd < 0)
            fn.bodyEnd = int(t.size());
        f.functions.append(fn);
    }
}

QVector<SpringParam> constructorProps(const FileData &f, int open, int close)
{
    QVector<SpringParam> out;
    static const QRegularExpression prop(QStringLiteral(R"(^\s*(?:@\w+(?:\([^)]*\))?\s*)*(?:override\s+)?(?:val|var)\s+(\w+)\s*:\s*([\w.<>?, ]+?)\s*(?:=\s*(.+?))?\s*$)"),
                                         QRegularExpression::DotMatchesEverythingOption);
    for (const auto &a : splitArgs(f.masked, open + 1, close)) {
        const QString piece = f.noComments.mid(a.first, a.second - a.first);
        const QRegularExpressionMatch m = prop.match(piece);
        if (!m.hasMatch() || m.captured(1) == QLatin1String("parent"))
            continue;
        SpringParam p;
        p.kind = SpringParam::Query;
        p.name = m.captured(1);
        p.type = m.captured(2).trimmed();
        QString def = m.captured(3).trimmed();
        if (def.startsWith(QLatin1Char('"')) && def.endsWith(QLatin1Char('"')))
            def = def.mid(1, def.size() - 2);
        if (def == QLatin1String("null"))
            def.clear();
        p.defaultValue = def;
        p.required = m.captured(3).isEmpty() && !p.type.endsWith(QLatin1Char('?'));
        out << p;
    }
    return out;
}

void findResources(FileData &f)
{
    static const QRegularExpression res(QStringLiteral(R"(@Resource\s*\()"));
    static const QRegularExpression cls(QStringLiteral(R"(\G\s*(?:@\w+(?:\([^)]*\))?\s*)*(?:(?:data|private|internal|public|open|sealed)\s+)*class\s+(\w+))"));
    const QString &t = f.masked;
    for (QRegularExpressionMatchIterator it = res.globalMatch(t); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int open = m.capturedEnd() - 1;
        const int close = closeOf(t, open);
        if (close < 0)
            continue;
        ResourceClass r;
        const int q = skipSpace(t, open + 1);
        if (const Literal *lit = f.literalAt(q))
            r.ownPath = lit->value;
        const QRegularExpressionMatch cm = cls.match(t, close + 1);
        if (!cm.hasMatch())
            continue;
        r.name = cm.captured(1);
        r.at = cm.capturedStart(1);
        int i = skipSpace(t, cm.capturedEnd());
        if (i < t.size() && t.at(i) == QLatin1Char('(')) {
            const int c = closeOf(t, i);
            if (c < 0)
                continue;
            r.props = constructorProps(f, i, c);
            i = c + 1;
        }
        // Optional supertype list, then the body.
        while (i < t.size() && t.at(i) != QLatin1Char('{') && t.at(i) != QLatin1Char('\n') && t.at(i) != QLatin1Char('@'))
            ++i;
        if (i < t.size() && t.at(i) == QLatin1Char('{')) {
            r.bodyOpen = i;
            r.bodyClose = closeOf(t, i);
        }
        f.resources.append(r);
    }
    // Nested classes: Articles.Id sits in the body of Articles and its path continues the outer one.
    for (ResourceClass &r : f.resources) {
        QStringList chain{r.name};
        QString path = r.ownPath;
        int pos = r.at;
        for (;;) {
            const ResourceClass *outer = nullptr;
            for (const ResourceClass &o : std::as_const(f.resources))
                if (o.bodyOpen >= 0 && pos > o.bodyOpen && pos < o.bodyClose && (!outer || o.bodyOpen > outer->bodyOpen))
                    outer = &o;
            if (!outer)
                break;
            chain.prepend(outer->name);
            path = joinPath(outer->ownPath, path);
            pos = outer->at;
        }
        r.qualified = chain.join(QLatin1Char('.'));
        r.path = joinPath(QString(), path);
    }
}

FileData parse(const QString &path, const QString &text)
{
    FileData f;
    f.path = path;
    f.text = text;
    f.lineStarts << 0;
    for (int i = 0; i < text.size(); ++i)
        if (text.at(i) == QLatin1Char('\n'))
            f.lineStarts << i + 1;
    mask(f);
    static const QRegularExpression pkg(QStringLiteral(R"(^\s*package\s+([\w.]+))"), QRegularExpression::MultilineOption);
    f.pkg = pkg.match(f.masked).captured(1);
    static const QRegularExpression jvmName(QStringLiteral(R"re(@file\s*:\s*JvmName\s*\(\s*"(\w+)")re"));
    f.jvmName = jvmName.match(f.noComments).captured(1);
    findSites(f);
    findFunctions(f);
    findResources(f);
    return f;
}

struct Mount
{
    QString prefix;
    QStringList auth;
};

class Resolver
{
public:
    explicit Resolver(QVector<FileData> &files) : m_files(files)
    {
        static const QRegularExpression constVal(QStringLiteral(R"re(\bconst\s+val\s+(\w+)\s*(?::\s*String\s*)?=\s*"([^"]*)")re"));
        for (int fi = 0; fi < m_files.size(); ++fi) {
            const FileData &f = m_files[fi];
            for (QRegularExpressionMatchIterator it = constVal.globalMatch(f.noComments); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                m_consts.insert(m.captured(1), m.captured(2));
            }
            for (int k = 0; k < f.functions.size(); ++k) {
                const Function &fn = f.functions[k];
                if (fn.receiver == QLatin1String("Route") || fn.receiver == QLatin1String("Routing"))
                    m_routeFns.insert(fn.name, {fi, k});
            }
            for (const ResourceClass &r : f.resources) {
                m_resources.insert(r.qualified, r);
                m_resourcesBySimple.insert(r.name, r);
            }
        }
        findCalls();
    }

    // The path written as the site's first argument (or `path = …`), "" when it has none.
    QString pathArg(const FileData &f, const Site &s) const
    {
        if (s.argsOpen < 0)
            return {};
        const auto args = splitArgs(f.masked, s.argsOpen + 1, s.argsClose);
        int pick = -1;
        for (int k = 0; k < args.size(); ++k) {
            const QString a = f.masked.mid(args[k].first, args[k].second - args[k].first).trimmed();
            static const QRegularExpression named(QStringLiteral(R"(^(\w+)\s*=)"));
            const QRegularExpressionMatch nm = named.match(a);
            if (nm.hasMatch()) {
                if (nm.captured(1) == QLatin1String("path") || nm.captured(1) == QLatin1String("remotePath")) {
                    pick = k;
                    break;
                }
                continue;
            }
            if (pick < 0)
                pick = k;
        }
        if (pick < 0)
            return {};
        int from = args[pick].first;
        const QString a = f.masked.mid(from, args[pick].second - from);
        const int eq = a.indexOf(QLatin1Char('='));
        static const QRegularExpression isNamed(QStringLiteral(R"(^\s*\w+\s*=)"));
        if (eq >= 0 && isNamed.match(a).hasMatch())
            from += eq + 1;
        from = skipSpace(f.masked, from);
        if (const Literal *lit = f.literalAt(from))
            return lit->value;
        const QString ident = f.masked.mid(from, args[pick].second - from).trimmed();
        static const QRegularExpression id(QStringLiteral(R"(^[\w.]+$)"));
        if (!id.match(ident).hasMatch())
            return {};
        const QString simple = ident.mid(ident.lastIndexOf(QLatin1Char('.')) + 1);
        if (simple.startsWith(QLatin1String("HttpMethod")) || ident.startsWith(QLatin1String("HttpMethod.")))
            return {};
        return m_consts.contains(simple) ? m_consts.value(simple) : QStringLiteral("{%1}").arg(simple);
    }

    QString methodArg(const FileData &f, const Site &s) const
    {
        if (s.argsOpen < 0)
            return {};
        static const QRegularExpression m(QStringLiteral(R"(HttpMethod\.(\w+))"));
        return m.match(f.masked.mid(s.argsOpen, s.argsClose - s.argsOpen)).captured(1).toUpper();
    }

    QString authArg(const FileData &f, const Site &s) const
    {
        QStringList names;
        if (s.argsOpen >= 0)
            for (const auto &a : splitArgs(f.masked, s.argsOpen + 1, s.argsClose)) {
                const int q = skipSpace(f.masked, a.first);
                if (const Literal *lit = f.literalAt(q))
                    names << lit->value;
            }
        return names.isEmpty() ? QString() : names.join(QStringLiteral(" | "));
    }

    // Prefix, auth and method of the route blocks around `pos` inside the same function.
    Mount local(const FileData &f, int pos, const Function *fn, QString *method = nullptr) const
    {
        Mount m;
        QVector<const Site *> around;
        for (const Site &s : f.sites)
            if (s.contains(pos) && (!fn || (s.at > fn->bodyStart && s.at < fn->bodyEnd)))
                around << &s;
        std::sort(around.begin(), around.end(), [](const Site *a, const Site *b) { return a->lambdaOpen < b->lambdaOpen; });
        for (const Site *s : std::as_const(around)) {
            if (s->name == QLatin1String("route")) {
                m.prefix = joinPath(m.prefix, pathArg(f, *s));
                if (method && !methodArg(f, *s).isEmpty())
                    *method = methodArg(f, *s);
            } else if (s->name == QLatin1String("authenticate")) {
                m.auth << authArg(f, *s);
            } else if (s->name == QLatin1String("method") && method) {
                *method = methodArg(f, *s);
            }
        }
        return m;
    }

    // Where a `fun Route.x()` ends up: one mount per call path from routing { } down to it.
    QVector<Mount> mountsOf(int fi, int k, int depth = 0)
    {
        const QPair<int, int> key{fi, k};
        if (m_mountCache.contains(key))
            return m_mountCache.value(key);
        if (depth > 12 || m_visiting.contains(key))
            return {};
        m_visiting.insert(key);
        QVector<Mount> out;
        for (const Call &c : m_calls.value(key)) {
            const FileData &f = m_files[c.file];
            const Function *caller = f.functionAt(c.at);
            const Mount here = local(f, c.at, caller);
            const bool callerIsRoute = caller && (caller->receiver == QLatin1String("Route") || caller->receiver == QLatin1String("Routing"));
            if (!callerIsRoute) {
                out << here;
            } else {
                const int ck = int(caller - f.functions.constData());
                for (const Mount &up : mountsOf(c.file, ck, depth + 1)) {
                    Mount m{joinPath(up.prefix, here.prefix), up.auth + here.auth};
                    if (up.prefix.isEmpty() || up.prefix == QLatin1String("/"))
                        m.prefix = here.prefix;
                    out << m;
                }
            }
            if (out.size() >= 8)
                break;
        }
        m_visiting.remove(key);
        m_mountCache.insert(key, out);
        return out;
    }

    const ResourceClass *resource(const QString &generic) const
    {
        if (generic.isEmpty())
            return nullptr;
        auto it = m_resources.constFind(generic);
        if (it != m_resources.constEnd())
            return &it.value();
        auto s = m_resourcesBySimple.constFind(generic.mid(generic.lastIndexOf(QLatin1Char('.')) + 1));
        return s != m_resourcesBySimple.constEnd() ? &s.value() : nullptr;
    }

    bool isRouteFunction(const QString &name) const { return m_routeFns.contains(name); }

private:
    struct Call
    {
        int file = 0, at = 0;
    };

    // Calls of `fun Route.x()` functions: x() or x(args), not a member call and not the declaration.
    void findCalls()
    {
        if (m_routeFns.isEmpty())
            return;
        QStringList names(m_routeFns.keyBegin(), m_routeFns.keyEnd());
        for (QString &n : names)
            n = QRegularExpression::escape(n);
        const QRegularExpression call(QStringLiteral(R"(\b(%1)\s*\()").arg(names.join(QLatin1Char('|'))));
        for (int fi = 0; fi < m_files.size(); ++fi) {
            const QString &t = m_files[fi].masked;
            for (QRegularExpressionMatchIterator it = call.globalMatch(t); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                int b = m.capturedStart() - 1;
                while (b >= 0 && (t.at(b) == QLatin1Char(' ') || t.at(b) == QLatin1Char('\t')))
                    --b;
                if (b >= 0 && (t.at(b) == QLatin1Char('.') || t.at(b) == QLatin1Char(':')))
                    continue;
                if (b >= 2 && t.mid(b - 2, 3) == QLatin1String("fun"))
                    continue;
                // Every function of that name (overloads in different files) is mounted here.
                for (const QPair<int, int> &target : m_routeFns.values(m.captured(1)))
                    m_calls[target].append({fi, int(m.capturedStart())});
            }
        }
    }

    QVector<FileData> &m_files;
    QHash<QString, QString> m_consts;
    QMultiHash<QString, QPair<int, int>> m_routeFns; // name -> (file, function index)
    QHash<QPair<int, int>, QVector<Call>> m_calls;
    QHash<QPair<int, int>, QVector<Mount>> m_mountCache;
    QSet<QPair<int, int>> m_visiting;
    QHash<QString, ResourceClass> m_resources, m_resourcesBySimple;
};

void handlerParams(const FileData &f, const Site &s, KtorRoute &r)
{
    QVector<SpringParam> &params = r.ep.params;
    auto add = [&params](SpringParam::Kind kind, const QString &name, const QString &type, const QString &def, bool required) {
        for (SpringParam &p : params)
            if (p.name == name && (p.kind == kind || (kind == SpringParam::Query && p.kind == SpringParam::Path))) {
                if (p.defaultValue.isEmpty())
                    p.defaultValue = def;
                return;
            }
        SpringParam p;
        p.kind = kind;
        p.name = name;
        p.type = type;
        p.defaultValue = def;
        p.required = required;
        params << p;
    };
    // Path variables: {id}, {id?}; a tailcard {path...} matches the rest of the path.
    static const QRegularExpression pathVar(QStringLiteral(R"(\{(\w+)(\?|\.\.\.)?\})"));
    for (QRegularExpressionMatchIterator it = pathVar.globalMatch(r.ep.path); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        add(SpringParam::Path, m.captured(1), QStringLiteral("String"), QString(), m.captured(2).isEmpty());
    }
    if (!s.hasLambda())
        return;
    const QString body = f.noComments.mid(s.lambdaOpen, s.lambdaClose - s.lambdaOpen);
    static const QRegularExpression query(QStringLiteral(R"re(\b(?:call\.)?(?:request\.)?queryParameters\s*\[\s*"([\w.\-]+)"\s*\](?:\s*\?:\s*(?:"([^"]*)"|([\w.\-]+)))?)re"));
    static const QRegularExpression params2(QStringLiteral(R"re(\bcall\.(?:parameters|pathParameters)\s*\[\s*"([\w.\-]+)"\s*\](?:\s*\?:\s*(?:"([^"]*)"|([\w.\-]+)))?)re"));
    static const QRegularExpression getOrFail(QStringLiteral(R"re(\b(query|path)?(?:Parameters|parameters)\s*\.\s*getOrFail(?:<\w+>)?\s*\(\s*"([\w.\-]+)")re"));
    static const QRegularExpression header(QStringLiteral(R"re(\bcall\.request\.(?:headers\s*\[\s*"([\w\-]+)"|header\s*\(\s*"([\w\-]+)"))re"));
    static const QRegularExpression receive(QStringLiteral(R"(\bcall\.receive(?:Nullable)?\s*<\s*([\w.<>, ?]+?)\s*>\s*\()"));
    static const QRegularExpression receiveTyped(QStringLiteral(R"(:\s*([\w.<>?]+)\s*=\s*call\.receive(?:Nullable)?\s*\(\s*\))"));
    static const QRegularExpression receiveText(QStringLiteral(R"(\bcall\.receiveText\s*\(\s*\))"));
    auto def = [](const QRegularExpressionMatch &m, int a, int b) { return !m.captured(a).isEmpty() ? m.captured(a) : m.captured(b); };
    for (QRegularExpressionMatchIterator it = query.globalMatch(body); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        add(SpringParam::Query, m.captured(1), QStringLiteral("String"), def(m, 2, 3), false);
    }
    for (QRegularExpressionMatchIterator it = params2.globalMatch(body); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const bool inPath = r.ep.path.contains(QLatin1Char('{') + m.captured(1));
        add(inPath ? SpringParam::Path : SpringParam::Query, m.captured(1), QStringLiteral("String"), def(m, 2, 3), false);
    }
    for (QRegularExpressionMatchIterator it = getOrFail.globalMatch(body); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const bool inPath = m.captured(1) == QLatin1String("path") || r.ep.path.contains(QLatin1Char('{') + m.captured(2));
        add(inPath ? SpringParam::Path : SpringParam::Query, m.captured(2), QStringLiteral("String"), QString(), true);
    }
    for (QRegularExpressionMatchIterator it = header.globalMatch(body); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        add(SpringParam::Header, def(m, 1, 2), QStringLiteral("String"), QString(), false);
    }
    QRegularExpressionMatch m = receive.match(body);
    if (!m.hasMatch())
        m = receiveTyped.match(body);
    if (m.hasMatch())
        add(SpringParam::Body, QStringLiteral("body"), m.captured(1).trimmed(), QString(), true);
    else if (receiveText.match(body).hasMatch())
        add(SpringParam::Body, QStringLiteral("body"), QStringLiteral("String"), QString(), true);
}

} // namespace

namespace KtorSource {

KtorScan scanTexts(const QList<QPair<QString, QString>> &input)
{
    KtorScan out;
    QVector<FileData> files;
    for (const auto &p : input) {
        files << parse(p.first, p.second);
        SpringEndpoints::parseFile(p.first, p.second, out.http); // Kotlin classes for the body skeletons
    }
    out.http.endpoints.clear(); // a Ktor project has no Spring controllers; keep the list to Ktor's routes
    out.files = int(files.size());
    Resolver resolver(files);
    QString embeddedModule; // module = X::name of embeddedServer(…)

    for (int fi = 0; fi < files.size(); ++fi) {
        const FileData &f = files[fi];
        const QString fileKt = !f.jvmName.isEmpty() ? f.jvmName : QFileInfo(f.path).completeBaseName().left(1).toUpper() + QFileInfo(f.path).completeBaseName().mid(1) + QStringLiteral("Kt");

        for (const Function &fn : f.functions)
            if (fn.receiver == QLatin1String("Application"))
                out.modules.append({fn.name, (f.pkg.isEmpty() ? QString() : f.pkg + QLatin1Char('.')) + fileKt + QLatin1Char('.') + fn.name, f.path, fn.line});

        for (const Site &s : f.sites) {
            const Function *fn = f.functionAt(s.at);
            const QString owner = fn ? fn->name : QFileInfo(f.path).fileName();
            if (s.name == QLatin1String("install")) {
                if (s.argsOpen < 0)
                    continue;
                QString plugin = f.masked.mid(s.argsOpen + 1, s.argsClose - s.argsOpen - 1).section(QLatin1Char(','), 0, 0).trimmed();
                plugin = plugin.mid(plugin.lastIndexOf(QLatin1Char('.')) + 1);
                if (!plugin.isEmpty())
                    out.installs.append({plugin, owner, f.path, f.lineAt(s.at)});
                continue;
            }

            KtorRoute r;
            QString own;
            const bool verb = verbs().contains(s.name);
            if (verb || s.name == QLatin1String("handle")) {
                if (!s.hasLambda())
                    continue;
                r.kind = KtorRoute::Http;
                r.ep.method = verb ? s.name.toUpper() : QString();
                if (const ResourceClass *res = resolver.resource(s.generic)) {
                    own = res->path;
                    r.resource = res->qualified;
                    for (SpringParam p : res->props) {
                        if (own.contains(QLatin1Char('{') + p.name)) {
                            p.kind = SpringParam::Path;
                            p.required = true;
                        }
                        r.ep.params << p;
                    }
                } else {
                    own = resolver.pathArg(f, s);
                }
            } else if (s.name == QLatin1String("webSocket") || s.name == QLatin1String("webSocketRaw")) {
                r.kind = KtorRoute::WebSocket;
                r.ep.method = QStringLiteral("WS");
                own = resolver.pathArg(f, s);
            } else if (s.name == QLatin1String("sse")) {
                r.kind = KtorRoute::Sse;
                r.ep.method = QStringLiteral("SSE");
                own = resolver.pathArg(f, s);
            } else if (s.name.startsWith(QLatin1String("static")) || s.name == QLatin1String("singlePageApplication")) {
                r.kind = KtorRoute::Static;
                r.ep.method = QStringLiteral("GET");
                own = resolver.pathArg(f, s);
            } else if (s.name == QLatin1String("swaggerUI") || s.name == QLatin1String("openAPI")) {
                r.kind = KtorRoute::Docs;
                r.ep.method = QStringLiteral("GET");
                own = resolver.pathArg(f, s);
                if (own.isEmpty())
                    own = s.name == QLatin1String("swaggerUI") ? QStringLiteral("swagger") : QStringLiteral("openapi");
            } else {
                continue;
            }

            QString method = r.ep.method;
            const Mount here = resolver.local(f, s.at, fn, &method);
            if (r.ep.method.isEmpty())
                r.ep.method = method.isEmpty() ? QStringLiteral("ANY") : method;
            QVector<Mount> mounts{Mount()};
            if (fn && (fn->receiver == QLatin1String("Route") || fn->receiver == QLatin1String("Routing"))) {
                mounts = resolver.mountsOf(fi, int(fn - f.functions.constData()));
                if (mounts.isEmpty()) {
                    mounts = {Mount()};
                    r.mounted = false;
                }
            }
            r.ep.controller = owner;
            r.ep.file = f.path;
            r.ep.line = f.lineAt(s.at);
            for (const Mount &m : std::as_const(mounts)) {
                KtorRoute one = r;
                one.ep.path = joinPath(joinPath(m.prefix, here.prefix), own);
                one.auth = m.auth + here.auth;
                handlerParams(f, s, one);
                QStringList detail;
                if (one.kind != KtorRoute::Http)
                    detail << one.kindName();
                if (!one.auth.isEmpty()) {
                    QStringList shown;
                    for (const QString &a : std::as_const(one.auth))
                        shown << (a.isEmpty() ? QStringLiteral("default") : a);
                    detail << QStringLiteral("auth: ") + shown.join(QStringLiteral(", "));
                }
                if (!one.mounted)
                    detail << QStringLiteral("not mounted");
                one.ep.handler = detail.join(QStringLiteral(" · "));
                out.routes << one;
            }
        }

        // Authentication providers: jwt("auth-jwt") { … } inside install(Authentication) / authentication { }.
        static const QRegularExpression provider(QStringLiteral(R"(\b(jwt|basic|form|session|oauth|bearer|digest|ldap|apiKey)\s*(?:<[^>{}()]*>)?\s*\()"));
        for (const Site &s : f.sites) {
            const bool auth = (s.name == QLatin1String("install") && s.argsOpen >= 0
                               && f.masked.mid(s.argsOpen, s.argsClose - s.argsOpen).contains(QStringLiteral("Authentication")))
                              || s.name == QLatin1String("authentication");
            if (!auth || !s.hasLambda())
                continue;
            const QString inner = f.masked.mid(s.lambdaOpen, s.lambdaClose - s.lambdaOpen);
            for (QRegularExpressionMatchIterator it = provider.globalMatch(inner); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                const int open = s.lambdaOpen + int(m.capturedEnd()) - 1;
                QString name;
                if (const Literal *lit = f.literalAt(skipSpace(f.masked, open + 1)))
                    name = lit->value;
                out.authProviders.append({m.captured(1), name, f.path, f.lineAt(s.lambdaOpen + int(m.capturedStart()))});
            }
        }

        // embeddedServer(Netty, port = 8080, …) / embeddedServer(CIO, 8080)
        static const QRegularExpression embedded(QStringLiteral(R"(\bembeddedServer\s*\(\s*(\w+)\s*(?:,\s*(?:port\s*=\s*)?(\d{2,5})\b|,[^)]*?\bport\s*=\s*(\d{2,5})\b)?)"));
        const QRegularExpressionMatch em = embedded.match(f.masked);
        if (em.hasMatch() && out.embeddedEngine.isEmpty()) {
            out.embeddedEngine = em.captured(1);
            out.embeddedPort = (!em.captured(2).isEmpty() ? em.captured(2) : em.captured(3)).toInt();
            out.embeddedFile = f.path;
            out.embeddedLine = f.lineAt(int(em.capturedStart()));
            // Which module Ktor would load again on a reload: module = X::name (checked against the suspend functions
            // once every file is read). A trailing lambda or module = { … } never is.
            const int open = f.masked.indexOf(QLatin1Char('('), em.capturedStart());
            const int close = open >= 0 ? closeOf(f.masked, open) : -1;
            if (close > 0) {
                static const QRegularExpression moduleArg(QStringLiteral(R"(^\s*module\s*=\s*(?:[\w.]*)::(\w+)\s*$)"));
                for (const auto &[from, to] : splitArgs(f.masked, open + 1, close)) {
                    const QRegularExpressionMatch mm = moduleArg.match(f.masked.mid(from, to - from));
                    if (mm.hasMatch())
                        embeddedModule = mm.captured(1);
                }
                const int after = skipSpace(f.masked, close + 1);
                if (after < f.masked.size() && f.masked.at(after) == QLatin1Char('{'))
                    embeddedModule.clear();
            }
        }
    }

    if (!embeddedModule.isEmpty()) {
        const QRegularExpression suspendModule(QStringLiteral(R"(\bsuspend\s+fun\s+Application\s*\.\s*%1\s*\()").arg(QRegularExpression::escape(embeddedModule)));
        for (const FileData &f : std::as_const(files))
            out.embeddedModuleReloadable = out.embeddedModuleReloadable || suspendModule.match(f.masked).hasMatch();
    }

    std::stable_sort(out.routes.begin(), out.routes.end(), [](const KtorRoute &a, const KtorRoute &b) {
        if (a.ep.file != b.ep.file)
            return a.ep.file < b.ep.file;
        return a.ep.line < b.ep.line;
    });
    for (const KtorRoute &r : std::as_const(out.routes))
        if (r.kind == KtorRoute::Http)
            out.http.endpoints << r.ep;
    return out;
}

KtorScan scan(const QStringList &sourceRoots)
{
    QList<QPair<QString, QString>> input;
    for (const QString &root : sourceRoots) {
        // Test sources use testApplication { routing { … } } for throwaway routes: they are not the server's.
        if (root.contains(QStringLiteral("/src/test/")))
            continue;
        QDirIterator it(root, {QStringLiteral("*.kt")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            if (f.size() > 1024 * 1024 || !f.open(QIODevice::ReadOnly))
                continue;
            input.append({path, TextEncoding::decode(f.readAll()).text});
        }
    }
    return scanTexts(input);
}

} // namespace KtorSource
