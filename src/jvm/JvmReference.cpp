#include "jvm/JvmReference.h"

#include "jvm/JvmTools.h"

#include <QList>
#include <QRegularExpression>

namespace JvmReference {

namespace {

bool isIdent(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

// Comments and the contents of string / char literals become spaces (offsets and line breaks kept), so braces and
// keywords inside them do not count.
QString masked(const QString &text)
{
    QString out = text;
    const int n = out.size();
    auto blank = [&out](int from, int to) {
        for (int k = from; k < to; ++k)
            if (out.at(k) != QLatin1Char('\n'))
                out[k] = QLatin1Char(' ');
    };
    for (int i = 0; i < n; ++i) {
        const QChar c = out.at(i);
        if (c == QLatin1Char('/') && i + 1 < n && out.at(i + 1) == QLatin1Char('/')) {
            const int end = out.indexOf(QLatin1Char('\n'), i);
            blank(i, end < 0 ? n : end);
            i = end < 0 ? n : end;
        } else if (c == QLatin1Char('/') && i + 1 < n && out.at(i + 1) == QLatin1Char('*')) {
            const int end = out.indexOf(QStringLiteral("*/"), i + 2);
            const int stop = end < 0 ? n : end + 2;
            blank(i, stop);
            i = stop - 1;
        } else if (c == QLatin1Char('"') && out.mid(i, 3) == QLatin1String("\"\"\"")) {
            const int end = out.indexOf(QStringLiteral("\"\"\""), i + 3);
            const int stop = end < 0 ? n : end + 3;
            blank(i + 3, qMax(i + 3, stop - 3));
            i = stop - 1;
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            int j = i + 1;
            for (; j < n && out.at(j) != c && out.at(j) != QLatin1Char('\n'); ++j)
                if (out.at(j) == QLatin1Char('\\'))
                    ++j;
            blank(i + 1, qMin(j, n));
            i = j;
        }
    }
    return out;
}

struct Decl {
    QString name;
    QString qualified; // Outer.Inner (no package)
    bool type = false; // class / interface / object / enum / record; else a function or property
    int nameStart = 0;
    int bodyStart = -1, bodyEnd = -1; // the braces, -1 when it has no body
};

// Declarations with their bodies. A type's members are found inside its braces; functions are kept so a caret in a
// function body can name it.
QList<Decl> declarations(const QString &m, bool kotlin)
{
    static const QRegularExpression ktDecl(QStringLiteral(
        R"re(\b(?:(class|interface|object)\s+(`?\w+`?)|(companion\s+object)\b(?:\s+(\w+))?|(fun)\s+(?:<[^>]*>\s*)?(?:[\w.<>?,\s]+\.)?(`?\w+`?)\s*\(|(val|var)\s+(?:<[^>]*>\s*)?(?:[\w.<>?]+\.)?(`?\w+`?)))re"));
    static const QRegularExpression javaDecl(QStringLiteral(
        R"re(\b(?:(class|interface|enum|record|@interface)\s+(\w+)|([\w<>\[\],.?]+)\s+(\w+)\s*\([^;{}]*\)\s*(?:throws\s+[\w.,\s]+)?(\{|;)))re"));
    QList<Decl> out;
    const QRegularExpression &re = kotlin ? ktDecl : javaDecl;
    QRegularExpressionMatchIterator it = re.globalMatch(m);
    while (it.hasNext()) {
        const QRegularExpressionMatch x = it.next();
        Decl d;
        int after = int(x.capturedEnd());
        if (kotlin) {
            if (!x.captured(2).isEmpty()) {
                d.type = true;
                d.name = x.captured(2);
                d.nameStart = int(x.capturedStart(2));
            } else if (!x.captured(3).isEmpty()) {
                d.type = true;
                d.name = x.captured(4).isEmpty() ? QStringLiteral("Companion") : x.captured(4);
                d.nameStart = int(x.capturedStart(x.captured(4).isEmpty() ? 3 : 4));
            } else if (!x.captured(5).isEmpty()) {
                d.name = x.captured(6);
                d.nameStart = int(x.capturedStart(6));
            } else {
                d.name = x.captured(8);
                d.nameStart = int(x.capturedStart(8));
            }
            d.name.remove(QLatin1Char('`'));
        } else {
            if (!x.captured(2).isEmpty()) {
                d.type = true;
                d.name = x.captured(2);
                d.nameStart = int(x.capturedStart(2));
            } else {
                // `return foo(x);`, `new Foo(…)`, `if (…) {` are statements, not declarations.
                static const QStringList keywords = {QStringLiteral("if"),     QStringLiteral("for"),   QStringLiteral("while"),
                                                     QStringLiteral("switch"), QStringLiteral("catch"), QStringLiteral("return"),
                                                     QStringLiteral("new"),    QStringLiteral("throw"), QStringLiteral("else"),
                                                     QStringLiteral("case"),   QStringLiteral("synchronized"), QStringLiteral("yield")};
                d.name = x.captured(4);
                d.nameStart = int(x.capturedStart(4));
                if (keywords.contains(d.name) || keywords.contains(x.captured(3)))
                    continue;
                after = int(x.capturedStart(5)); // the { or ;
            }
        }
        // The body: the next `{` before the declaration ends (Kotlin: a line that does not continue the header;
        // Java and functions: a `;` or `=`).
        // A Kotlin function's match ends inside its parameter list.
        int paren = kotlin && !d.type && x.captured(0).endsWith(QLatin1Char('(')) ? 1 : 0;
        for (int i = after; i < m.size(); ++i) {
            const QChar c = m.at(i);
            if (c == QLatin1Char('(') || c == QLatin1Char('<'))
                ++paren;
            else if ((c == QLatin1Char(')') || c == QLatin1Char('>')) && paren > 0)
                --paren;
            else if (paren > 0)
                continue;
            else if (c == QLatin1Char(')') || c == QLatin1Char(',')) // a constructor parameter / argument ends here
                break;
            else if (c == QLatin1Char('{')) {
                d.bodyStart = i;
                break;
            } else if (c == QLatin1Char(';') || c == QLatin1Char('}') || (c == QLatin1Char('=') && !d.type)) {
                break;
            } else if (c == QLatin1Char('\n') && kotlin) {
                int k = i + 1;
                while (k < m.size() && m.at(k).isSpace())
                    ++k;
                const QChar next = k < m.size() ? m.at(k) : QChar();
                if (!(next == QLatin1Char(':') || next == QLatin1Char('{') || next == QLatin1Char(',') || next == QLatin1Char('(')
                      || next == QLatin1Char(')') || m.mid(k, 5) == QLatin1String("where")))
                    break;
            }
        }
        if (d.bodyStart >= 0) {
            int depth = 0;
            for (int i = d.bodyStart; i < m.size(); ++i) {
                if (m.at(i) == QLatin1Char('{'))
                    ++depth;
                else if (m.at(i) == QLatin1Char('}') && --depth == 0) {
                    d.bodyEnd = i;
                    break;
                }
            }
            if (d.bodyEnd < 0)
                d.bodyEnd = int(m.size());
        }
        out << d;
    }
    // Qualified names of types: the types whose bodies hold them, outermost first. Local declarations (inside a
    // function body) are left without a qualified name.
    for (Decl &d : out) {
        QStringList chain;
        bool local = false;
        for (const Decl &o : std::as_const(out)) {
            if (&o == &d || o.bodyStart < 0 || !(o.bodyStart < d.nameStart && d.nameStart < o.bodyEnd))
                continue;
            if (!o.type)
                local = true;
            else
                chain << o.name;
        }
        if (!local)
            d.qualified = (chain + QStringList{d.name}).join(QLatin1Char('.'));
    }
    return out;
}

} // namespace

bool supports(const QString &path)
{
    return path.endsWith(QLatin1String(".kt")) || path.endsWith(QLatin1String(".kts")) || path.endsWith(QLatin1String(".java"));
}

QString at(const QString &path, const QString &text, int line, int column)
{
    if (!supports(path))
        return {};
    const bool kotlin = !path.endsWith(QLatin1String(".java"));
    const QString m = masked(text);
    // Caret offset.
    int offset = 0;
    for (int l = 0; l < line && offset >= 0; ++l) {
        offset = m.indexOf(QLatin1Char('\n'), offset);
        if (offset >= 0)
            ++offset;
    }
    if (offset < 0)
        return {};
    offset = qMin(int(m.size()), offset + column);
    int ws = offset, we = offset;
    while (ws > 0 && isIdent(m.at(ws - 1)))
        --ws;
    while (we < m.size() && isIdent(m.at(we)))
        ++we;
    const QString word = m.mid(ws, we - ws);

    const QString pkg = JvmTools::declaredPackage(text);
    auto withPkg = [&pkg](const QString &name) { return pkg.isEmpty() ? name : pkg + QLatin1Char('.') + name; };
    const QList<Decl> decls = declarations(m, kotlin);

    // The innermost type around a position, "" at the top level.
    auto ownerOf = [&decls](int pos) {
        const Decl *best = nullptr;
        for (const Decl &d : decls)
            if (d.type && d.bodyStart >= 0 && d.bodyStart < pos && pos < d.bodyEnd && !d.qualified.isEmpty()
                && (!best || d.bodyStart > best->bodyStart))
                best = &d;
        return best ? best->qualified : QString();
    };
    auto memberRef = [&](const Decl &d) {
        const QString owner = ownerOf(d.nameStart);
        if (owner.isEmpty()) // Kotlin top level: the package function / property
            return withPkg(d.name);
        return withPkg(owner) + QLatin1Char('#') + d.name;
    };

    if (!word.isEmpty()) {
        // 1. The caret is on a declaration's name.
        for (const Decl &d : decls)
            if (d.nameStart <= offset && offset <= d.nameStart + d.name.size() && !d.qualified.isEmpty())
                return d.type ? withPkg(d.qualified) : memberRef(d);
        // 2. A type declared in this file.
        for (const Decl &d : decls)
            if (d.type && d.name == word && !d.qualified.isEmpty())
                return withPkg(d.qualified);
        // 3. An import names it (or aliases it).
        static const QRegularExpression importRe(QStringLiteral(R"(^[ \t]*import\s+(?:static\s+)?([\w.`]+)(?:\s+as\s+(\w+))?)"),
                                                 QRegularExpression::MultilineOption);
        QRegularExpressionMatchIterator it = importRe.globalMatch(m);
        while (it.hasNext()) {
            const QRegularExpressionMatch x = it.next();
            QString fq = x.captured(1);
            fq.remove(QLatin1Char('`'));
            const QString alias = x.captured(2);
            if (alias == word || (alias.isEmpty() && fq.section(QLatin1Char('.'), -1) == word))
                return fq;
        }
        // 4. A member of this file named like the word (a call of a function declared here).
        for (const Decl &d : decls)
            if (!d.type && d.name == word && !d.qualified.isEmpty())
                return memberRef(d);
    }
    // 5. The declaration around the caret: the innermost function, else the type.
    const Decl *inner = nullptr;
    for (const Decl &d : decls)
        if (d.bodyStart >= 0 && d.bodyStart < offset && offset <= d.bodyEnd && !d.qualified.isEmpty()
            && (!inner || d.bodyStart > inner->bodyStart))
            inner = &d;
    if (inner)
        return inner->type ? withPkg(inner->qualified) : memberRef(*inner);
    return {};
}

} // namespace JvmReference
