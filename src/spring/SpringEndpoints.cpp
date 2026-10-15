#include "spring/SpringEndpoints.h"

#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

QString SpringEndpoint::bodyType() const
{
    for (const SpringParam &p : params)
        if (p.kind == SpringParam::Body)
            return p.type;
    return {};
}

QString SpringEndpoint::urlTemplate() const
{
    QStringList query;
    for (const SpringParam &p : params) {
        if (p.kind != SpringParam::Query)
            continue;
        if (p.required || !p.defaultValue.isEmpty())
            query << p.name + QLatin1Char('=') + p.defaultValue;
    }
    return query.isEmpty() ? path : path + QLatin1Char('?') + query.join(QLatin1Char('&'));
}

namespace {

// Index after the string / char literal that starts at i (Kotlin raw strings included).
int skipString(const QString &s, int i)
{
    const int n = s.size();
    if (s.mid(i, 3) == QLatin1String("\"\"\"")) {
        const int e = s.indexOf(QLatin1String("\"\"\""), i + 3);
        return e < 0 ? n : e + 3;
    }
    const QChar q = s.at(i);
    int j = i + 1;
    while (j < n && s.at(j) != q && s.at(j) != QLatin1Char('\n'))
        j += s.at(j) == QLatin1Char('\\') ? 2 : 1;
    return j < n && s.at(j) == q ? j + 1 : qMin(j, n);
}

// Comments become blanks (newlines stay) so offsets and line numbers still match the file.
QString stripComments(const QString &s)
{
    QString o = s;
    const int n = s.size();
    int i = 0;
    while (i < n) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('/')) {
            while (i < n && s.at(i) != QLatin1Char('\n'))
                o[i++] = QLatin1Char(' ');
        } else if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('*')) {
            int e = s.indexOf(QLatin1String("*/"), i + 2);
            e = e < 0 ? n : e + 2;
            for (; i < e; ++i)
                if (s.at(i) != QLatin1Char('\n'))
                    o[i] = QLatin1Char(' ');
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            i = skipString(s, i);
        } else {
            ++i;
        }
    }
    return o;
}

// Index of the bracket that closes the one at `open`, or -1.
int matchClose(const QString &t, int open)
{
    const QChar o = t.at(open);
    const QChar c = o == QLatin1Char('(') ? QLatin1Char(')') : o == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(']');
    int depth = 0;
    for (int i = open; i < t.size();) {
        const QChar ch = t.at(i);
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            i = skipString(t, i);
            continue;
        }
        if (ch == o)
            ++depth;
        else if (ch == c && --depth == 0)
            return i;
        ++i;
    }
    return -1;
}

QStringList splitTop(const QString &s)
{
    QStringList out;
    int depth = 0, start = 0;
    for (int i = 0; i < s.size();) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            i = skipString(s, i);
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{') || c == QLatin1Char('<'))
            ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'))
            --depth;
        else if (c == QLatin1Char('>') && !(i > 0 && s.at(i - 1) == QLatin1Char('-')))
            --depth;
        else if (c == QLatin1Char(',') && depth <= 0) {
            out << s.mid(start, i - start);
            start = i + 1;
        }
        ++i;
    }
    out << s.mid(start);
    QStringList trimmed;
    for (const QString &p : std::as_const(out))
        if (!p.trimmed().isEmpty())
            trimmed << p.trimmed();
    return trimmed;
}

struct Ann
{
    QString name;
    QString args;
    bool hasArgs = false;
    int end = 0;
};

// t[at] is '@'. Use-site targets (@field:Foo, @param:Foo) and qualified names reduce to the simple name.
bool parseAnnotation(const QString &t, int at, Ann &a)
{
    const int n = t.size();
    int i = at + 1;
    const int s = i;
    while (i < n && (t.at(i).isLetterOrNumber() || t.at(i) == QLatin1Char('_') || t.at(i) == QLatin1Char('.') || t.at(i) == QLatin1Char(':')))
        ++i;
    a.name = t.mid(s, i - s);
    a.name = a.name.mid(a.name.lastIndexOf(QLatin1Char(':')) + 1);
    a.name = a.name.mid(a.name.lastIndexOf(QLatin1Char('.')) + 1);
    int j = i;
    while (j < n && t.at(j) == QLatin1Char(' '))
        ++j;
    if (j < n && t.at(j) == QLatin1Char('(')) {
        const int e = matchClose(t, j);
        if (e < 0)
            return false;
        a.args = t.mid(j + 1, e - j - 1);
        a.hasArgs = true;
        a.end = e + 1;
    } else {
        a.end = i;
    }
    return !a.name.isEmpty();
}

QStringList literalsOf(const QString &v)
{
    static const QRegularExpression re(QStringLiteral(R"re("((?:[^"\\]|\\.)*)")re"));
    QStringList out;
    auto it = re.globalMatch(v);
    while (it.hasNext())
        out << it.next().captured(1);
    return out;
}

const QRegularExpression &keyedArg()
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*(\w+)\s*=(?!=)\s*(.*)$)"), QRegularExpression::DotMatchesEverythingOption);
    return re;
}

QString unquote(const QString &raw)
{
    const QString v = raw.trimmed();
    if (v.startsWith(QLatin1Char('"')) || v.startsWith(QLatin1Char('[')) || v.startsWith(QLatin1Char('{'))) {
        const QStringList l = literalsOf(v);
        if (!l.isEmpty())
            return l.first();
    }
    return v;
}

// `value` / `path` of a mapping annotation, or its unnamed string arguments.
QStringList mappingPaths(const QString &args)
{
    QStringList out;
    for (const QString &part : splitTop(args)) {
        const auto m = keyedArg().match(part);
        if (m.hasMatch()) {
            const QString key = m.captured(1);
            if (key == QLatin1String("value") || key == QLatin1String("path"))
                out << literalsOf(m.captured(2));
        } else {
            out << literalsOf(part);
        }
    }
    return out;
}

QStringList mappingMethods(const QString &args)
{
    static const QRegularExpression verbs(QStringLiteral(R"(\b(GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS)\b)"));
    QStringList out;
    for (const QString &part : splitTop(args)) {
        const auto m = keyedArg().match(part);
        if (!m.hasMatch() || m.captured(1) != QLatin1String("method"))
            continue;
        auto it = verbs.globalMatch(m.captured(2));
        while (it.hasNext())
            out << it.next().captured(1);
    }
    return out;
}

// Named argument of a parameter annotation; `value` also matches the unnamed first string.
QString argOf(const QString &args, const QStringList &keys, bool *found = nullptr)
{
    if (found)
        *found = false;
    for (const QString &part : splitTop(args)) {
        const auto m = keyedArg().match(part);
        if (m.hasMatch()) {
            if (keys.contains(m.captured(1))) {
                if (found)
                    *found = true;
                return unquote(m.captured(2));
            }
        } else if (keys.contains(QStringLiteral("value")) && part.startsWith(QLatin1Char('"'))) {
            if (found)
                *found = true;
            return unquote(part);
        }
    }
    return {};
}

QString joinPath(const QString &a, const QString &b)
{
    static const QRegularExpression slashes(QStringLiteral("/{2,}"));
    static const QRegularExpression regexVar(QStringLiteral(R"(\{(\w+):[^}]*\})"));
    QString r = QLatin1Char('/') + a.trimmed() + QLatin1Char('/') + b.trimmed();
    r.replace(regexVar, QStringLiteral("{\\1}"));
    r.replace(slashes, QStringLiteral("/"));
    if (r.size() > 1 && r.endsWith(QLatin1Char('/')))
        r.chop(1);
    return r;
}

int lineStart(const QString &t, int pos)
{
    return pos > 0 ? t.lastIndexOf(QLatin1Char('\n'), pos - 1) + 1 : 0;
}

bool isSimpleType(QString type)
{
    static const QSet<QString> simple{QStringLiteral("String"), QStringLiteral("Int"), QStringLiteral("Long"), QStringLiteral("Short"),
                                      QStringLiteral("Byte"), QStringLiteral("Boolean"), QStringLiteral("Double"), QStringLiteral("Float"),
                                      QStringLiteral("Integer"), QStringLiteral("int"), QStringLiteral("long"), QStringLiteral("boolean"),
                                      QStringLiteral("double"), QStringLiteral("float"), QStringLiteral("UUID"), QStringLiteral("BigDecimal"),
                                      QStringLiteral("LocalDate"), QStringLiteral("LocalDateTime")};
    if (type.endsWith(QLatin1Char('?')))
        type.chop(1);
    return simple.contains(type.trimmed());
}

QVector<SpringParam> parseParams(const QString &list, bool kotlin)
{
    QVector<SpringParam> out;
    for (const QString &part : splitTop(list)) {
        QString rest = part.trimmed();
        QVector<Ann> anns;
        int pos = 0;
        while (pos < rest.size()) {
            while (pos < rest.size() && rest.at(pos).isSpace())
                ++pos;
            Ann b;
            if (pos < rest.size() && rest.at(pos) == QLatin1Char('@') && parseAnnotation(rest, pos, b)) {
                anns << b;
                pos = b.end;
            } else {
                break;
            }
        }
        rest = rest.mid(pos).trimmed();

        QString name, type, dflt;
        bool hasDefault = false;
        if (kotlin) {
            const int eq = rest.indexOf(QLatin1Char('='));
            if (eq >= 0) {
                dflt = rest.mid(eq + 1).trimmed();
                hasDefault = true;
                rest = rest.left(eq).trimmed();
            }
            static const QRegularExpression mods(QStringLiteral(R"(^(?:vararg\s+|val\s+|var\s+|final\s+)+)"));
            rest.remove(mods);
            const int colon = rest.indexOf(QLatin1Char(':'));
            if (colon < 0)
                continue;
            name = rest.left(colon).trimmed();
            type = rest.mid(colon + 1).trimmed();
        } else {
            static const QRegularExpression fin(QStringLiteral(R"(^final\s+)"));
            rest.remove(fin);
            const int sp = rest.lastIndexOf(QRegularExpression(QStringLiteral(R"(\s)")));
            if (sp < 0)
                continue;
            name = rest.mid(sp + 1);
            type = rest.left(sp).trimmed();
        }

        SpringParam p;
        p.name = name;
        p.type = type;
        const Ann *found = nullptr;
        for (const Ann &a : std::as_const(anns)) {
            const QString n = a.name;
            if (n == QLatin1String("PathVariable")) {
                p.kind = SpringParam::Path;
                found = &a;
            } else if (n == QLatin1String("RequestParam")) {
                p.kind = SpringParam::Query;
                found = &a;
            } else if (n == QLatin1String("RequestHeader")) {
                p.kind = SpringParam::Header;
                found = &a;
            } else if (n == QLatin1String("RequestBody")) {
                p.kind = SpringParam::Body;
                found = &a;
            }
        }
        if (!found) {
            if (!isSimpleType(type))
                continue; // request, response, principal, model attribute ...
            p.kind = SpringParam::Query; // Spring binds plain simple-typed arguments as request parameters
        }
        if (found && p.kind != SpringParam::Body) {
            const QString explicitName = argOf(found->args, {QStringLiteral("value"), QStringLiteral("name")});
            if (!explicitName.isEmpty())
                p.name = explicitName;
            if (argOf(found->args, {QStringLiteral("required")}) == QLatin1String("false"))
                p.required = false;
            bool hasAnnDefault = false;
            const QString d = argOf(found->args, {QStringLiteral("defaultValue")}, &hasAnnDefault);
            if (hasAnnDefault) {
                p.required = false;
                p.defaultValue = d;
            }
        }
        if (p.kind == SpringParam::Query || p.kind == SpringParam::Header) {
            if (type.endsWith(QLatin1Char('?')) || type.startsWith(QLatin1String("Optional")))
                p.required = false;
            if (hasDefault) {
                p.required = false;
                if (p.defaultValue.isEmpty()) {
                    const QString lit = unquote(dflt);
                    static const QRegularExpression simpleLit(QStringLiteral(R"(^-?[\w.]+$)"));
                    if (dflt.startsWith(QLatin1Char('"')) || simpleLit.match(lit).hasMatch())
                        p.defaultValue = lit == QLatin1String("null") ? QString() : lit;
                }
            }
        }
        if (p.kind == SpringParam::Path)
            p.required = true;
        out << p;
    }
    return out;
}

// Constructor properties of Kotlin classes and components of Java records, for the JSON placeholder of a request body.
void collectTypes(const QString &t, bool kotlin, SpringScan &into)
{
    static const QRegularExpression ktClass(
        QStringLiteral(R"(\bclass\s+(\w+)\s*(?:<[^>{(]*>)?\s*(?:(?:private|internal|public|protected)\s+)?(?:constructor\s*)?\()"));
    static const QRegularExpression javaRecord(QStringLiteral(R"(\brecord\s+(\w+)\s*(?:<[^>{(]*>)?\s*\()"));
    auto it = (kotlin ? ktClass : javaRecord).globalMatch(t);
    while (it.hasNext()) {
        const auto m = it.next();
        const int open = m.capturedEnd() - 1;
        const int close = matchClose(t, open);
        if (close < 0)
            continue;
        QVector<SpringScan::Field> fields;
        for (const QString &part : splitTop(t.mid(open + 1, close - open - 1))) {
            QString rest = part;
            QString jsonName;
            int pos = 0;
            while (pos < rest.size()) {
                while (pos < rest.size() && rest.at(pos).isSpace())
                    ++pos;
                Ann a;
                if (pos < rest.size() && rest.at(pos) == QLatin1Char('@') && parseAnnotation(rest, pos, a)) {
                    if (a.name == QLatin1String("JsonProperty"))
                        jsonName = argOf(a.args, {QStringLiteral("value")});
                    pos = a.end;
                } else {
                    break;
                }
            }
            rest = rest.mid(pos).trimmed();
            SpringScan::Field f;
            if (kotlin) {
                static const QRegularExpression prop(QStringLiteral(R"(^(?:(?:private|public|internal|protected|override|open)\s+)*(?:val|var)\s+(\w+)\s*:\s*([^=]+?)\s*(?:=.*)?$)"),
                                                     QRegularExpression::DotMatchesEverythingOption);
                const auto pm = prop.match(rest);
                if (!pm.hasMatch())
                    continue;
                f.name = pm.captured(1);
                f.type = pm.captured(2).trimmed();
            } else {
                const int sp = rest.lastIndexOf(QRegularExpression(QStringLiteral(R"(\s)")));
                if (sp < 0)
                    continue;
                f.name = rest.mid(sp + 1);
                f.type = rest.left(sp).trimmed();
            }
            if (!jsonName.isEmpty())
                f.name = jsonName;
            fields << f;
        }
        if (!fields.isEmpty())
            into.types.insert(m.captured(1), fields);
    }
}

} // namespace

void SpringEndpoints::parseFile(const QString &path, const QString &text, SpringScan &into)
{
    const bool kotlin = path.endsWith(QLatin1String(".kt")) || path.endsWith(QLatin1String(".kts"));
    const QString t = stripComments(text);
    collectTypes(t, kotlin, into);

    QVector<int> starts{0};
    for (int i = 0; i < t.size(); ++i)
        if (t.at(i) == QLatin1Char('\n'))
            starts << i + 1;
    auto lineAt = [&starts](int off) { return int(std::upper_bound(starts.begin(), starts.end(), off) - starts.begin()); };

    static const QRegularExpression ctrl(QStringLiteral(R"(@(?:Rest)?Controller\b)"));
    static const QRegularExpression cls(QStringLiteral(R"(\b(?:class|interface)\s+(\w+))"));
    static const QRegularExpression reqMap(QStringLiteral(R"(@RequestMapping\b)"));
    static const QRegularExpression handlerMap(QStringLiteral(R"(@(Get|Post|Put|Delete|Patch|Request)Mapping\b)"));
    static const QRegularExpression lastWord(QStringLiteral(R"((\w+)\s*$)"));

    int lastEnd = 0;
    auto it = ctrl.globalMatch(t);
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.capturedStart() < lastEnd)
            continue;
        const auto cm = cls.match(t, m.capturedEnd());
        if (!cm.hasMatch())
            break;

        // Annotations on lines above @RestController belong to the class as well.
        int block = lineStart(t, m.capturedStart());
        while (block > 0) {
            const int prev = lineStart(t, block - 1);
            if (t.mid(prev, block - 1 - prev).trimmed().startsWith(QLatin1Char('@')))
                block = prev;
            else
                break;
        }
        QStringList bases;
        auto rm = reqMap.globalMatch(t.left(cm.capturedStart()), block);
        while (rm.hasNext()) {
            Ann a;
            if (parseAnnotation(t, rm.next().capturedStart(), a) && a.hasArgs)
                bases << mappingPaths(a.args);
        }
        if (bases.isEmpty())
            bases << QString();

        int open = cm.capturedEnd();
        while (open < t.size() && t.at(open) != QLatin1Char('{')) {
            if (t.at(open) == QLatin1Char('(')) {
                const int e = matchClose(t, open);
                if (e < 0)
                    break;
                open = e;
            }
            ++open;
        }
        if (open >= t.size())
            break;
        const int close = matchClose(t, open);
        if (close < 0)
            break;
        lastEnd = close;

        auto hm = handlerMap.globalMatch(t, open + 1);
        while (hm.hasNext()) {
            const auto h = hm.next();
            if (h.capturedStart() >= close)
                break;
            Ann a;
            if (!parseAnnotation(t, h.capturedStart(), a))
                continue;
            QStringList methods;
            const QString verb = h.captured(1).toUpper();
            if (verb == QLatin1String("REQUEST")) {
                methods = a.hasArgs ? mappingMethods(a.args) : QStringList();
                if (methods.isEmpty())
                    methods << QStringLiteral("ANY");
            } else {
                methods << verb;
            }
            QStringList paths = a.hasArgs ? mappingPaths(a.args) : QStringList();
            if (paths.isEmpty())
                paths << QString();

            int p = a.end;
            while (true) {
                while (p < t.size() && t.at(p).isSpace())
                    ++p;
                Ann b;
                if (p < t.size() && t.at(p) == QLatin1Char('@') && parseAnnotation(t, p, b))
                    p = b.end;
                else
                    break;
            }
            const int paren = t.indexOf(QLatin1Char('('), p);
            if (paren < 0 || paren >= close)
                continue;
            const QString between = t.mid(p, paren - p);
            if (between.contains(QLatin1Char('{')) || between.contains(QLatin1Char(';')) || between.contains(QLatin1Char('}')))
                continue;
            const auto wm = lastWord.match(between);
            const int pc = matchClose(t, paren);
            if (!wm.hasMatch() || pc < 0)
                continue;
            const QVector<SpringParam> params = parseParams(t.mid(paren + 1, pc - paren - 1), kotlin);

            int made = 0;
            for (const QString &base : std::as_const(bases))
                for (const QString &sub : std::as_const(paths))
                    for (const QString &method : std::as_const(methods)) {
                        if (made++ >= 24)
                            break;
                        SpringEndpoint e;
                        e.method = method;
                        e.path = joinPath(base, sub);
                        e.controller = cm.captured(1);
                        e.handler = wm.captured(1);
                        e.file = path;
                        e.line = lineAt(h.capturedStart());
                        e.params = params;
                        into.endpoints << e;
                    }
        }
    }
}

SpringScan SpringEndpoints::scan(const QStringList &sourceRoots)
{
    SpringScan result;
    for (const QString &root : sourceRoots) {
        QDirIterator it(root, {QStringLiteral("*.kt"), QStringLiteral("*.java")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            if (f.size() > 1024 * 1024 || !f.open(QIODevice::ReadOnly))
                continue;
            parseFile(path, QString::fromUtf8(f.readAll()), result);
        }
    }
    std::stable_sort(result.endpoints.begin(), result.endpoints.end(), [](const SpringEndpoint &a, const SpringEndpoint &b) {
        if (a.file != b.file)
            return a.file < b.file;
        return a.line < b.line;
    });
    return result;
}

namespace {

QString skeletonOf(const SpringScan &scan, QString type, int depth, const QString &indent)
{
    type = type.trimmed();
    if (type.endsWith(QLatin1Char('?')))
        type.chop(1);
    static const QRegularExpression generic(QStringLiteral(R"(^([\w.]+)\s*<(.*)>$)"), QRegularExpression::DotMatchesEverythingOption);
    QString base = type, arg;
    const auto gm = generic.match(type);
    if (gm.hasMatch()) {
        base = gm.captured(1);
        const QStringList args = splitTop(gm.captured(2));
        arg = args.isEmpty() ? QString() : args.first();
    }
    base = base.mid(base.lastIndexOf(QLatin1Char('.')) + 1);

    static const QSet<QString> text{QStringLiteral("String"), QStringLiteral("Char"), QStringLiteral("CharSequence"), QStringLiteral("UUID")};
    static const QSet<QString> whole{QStringLiteral("Int"), QStringLiteral("Long"), QStringLiteral("Short"), QStringLiteral("Byte"),
                                     QStringLiteral("Integer"), QStringLiteral("BigInteger"), QStringLiteral("int"), QStringLiteral("long"),
                                     QStringLiteral("short"), QStringLiteral("byte")};
    static const QSet<QString> fraction{QStringLiteral("Double"), QStringLiteral("Float"), QStringLiteral("BigDecimal"), QStringLiteral("double"),
                                        QStringLiteral("float")};
    static const QSet<QString> list{QStringLiteral("List"), QStringLiteral("Set"), QStringLiteral("Collection"), QStringLiteral("MutableList"),
                                    QStringLiteral("MutableSet"), QStringLiteral("Iterable"), QStringLiteral("ArrayList"), QStringLiteral("Array"),
                                    QStringLiteral("HashSet")};
    static const QSet<QString> map{QStringLiteral("Map"), QStringLiteral("MutableMap"), QStringLiteral("HashMap")};

    if (text.contains(base))
        return QStringLiteral("\"\"");
    if (whole.contains(base))
        return QStringLiteral("0");
    if (fraction.contains(base))
        return QStringLiteral("0.0");
    if (base == QLatin1String("Boolean") || base == QLatin1String("boolean"))
        return QStringLiteral("false");
    if (base == QLatin1String("LocalDate"))
        return QStringLiteral("\"2026-01-01\"");
    if (base == QLatin1String("LocalDateTime"))
        return QStringLiteral("\"2026-01-01T00:00:00\"");
    if (base == QLatin1String("Instant") || base == QLatin1String("OffsetDateTime") || base == QLatin1String("ZonedDateTime"))
        return QStringLiteral("\"2026-01-01T00:00:00Z\"");
    if (base == QLatin1String("Optional") && !arg.isEmpty())
        return skeletonOf(scan, arg, depth, indent);
    if (map.contains(base))
        return QStringLiteral("{}");
    const QString inner = indent + QStringLiteral("  ");
    if (list.contains(base)) {
        if (arg.isEmpty() || depth >= 4)
            return QStringLiteral("[]");
        return QStringLiteral("[\n") + inner + skeletonOf(scan, arg, depth + 1, inner) + QStringLiteral("\n") + indent + QLatin1Char(']');
    }
    const auto fields = scan.types.constFind(base);
    if (fields == scan.types.constEnd() || depth >= 4)
        return QStringLiteral("{}");
    QStringList lines;
    for (const SpringScan::Field &f : fields.value())
        lines << inner + QLatin1Char('"') + f.name + QStringLiteral("\": ") + skeletonOf(scan, f.type, depth + 1, inner);
    return QStringLiteral("{\n") + lines.join(QStringLiteral(",\n")) + QLatin1Char('\n') + indent + QLatin1Char('}');
}

} // namespace

QString SpringScan::bodySkeleton(const QString &type) const
{
    const QString s = skeletonOf(*this, type, 0, QString());
    return s.startsWith(QLatin1Char('{')) || s.startsWith(QLatin1Char('[')) ? s : QString();
}
