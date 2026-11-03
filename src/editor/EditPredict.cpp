#include "editor/EditPredict.h"

#include <QRegularExpression>
#include <QSet>

namespace EditPredict {
namespace {

constexpr int kMaxEdits = 300;
constexpr int kScanBack = 4000; // lines searched upwards for the enclosing function

bool isIdent(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$'); }

struct Scan {
    const QString &t;
    const Context &ctx;
    bool code(int i) const // a character the syntax highlighter did not put in a string or comment
    {
        return !(ctx.inString && ctx.inString(i)) && !(ctx.inComment && ctx.inComment(i));
    }
    bool inString(int i) const { return ctx.inString && ctx.inString(i); }
};

bool lang(const Context &ctx, std::initializer_list<const char *> names)
{
    for (const char *n : names)
        if (ctx.language == QLatin1String(n))
            return true;
    return false;
}

int lineStart(const QString &t, int pos)
{
    if (pos <= 0)
        return 0;
    return t.lastIndexOf(QLatin1Char('\n'), pos - 1) + 1;
}

int lineEnd(const QString &t, int pos)
{
    const int i = t.indexOf(QLatin1Char('\n'), pos);
    return i < 0 ? t.size() : i;
}

int indentOf(const QString &t, int ls)
{
    int n = 0;
    for (int i = ls; i < t.size(); ++i) {
        if (t.at(i) == QLatin1Char(' '))
            ++n;
        else if (t.at(i) == QLatin1Char('\t'))
            n += 4;
        else
            break;
    }
    return n;
}

bool blankLine(const QString &t, int ls)
{
    for (int i = ls; i < t.size() && t.at(i) != QLatin1Char('\n'); ++i)
        if (!t.at(i).isSpace())
            return false;
    return true;
}

// Length of the member operator (".", "->", "::") that ends at `end`, 0 for none.
int memberOpBefore(const QString &t, int end)
{
    if (end >= 1 && t.at(end - 1) == QLatin1Char('.'))
        return end >= 2 && t.at(end - 2) == QLatin1Char('.') ? 0 : 1; // not a spread `...x` / range `a..b`
    if (end >= 2 && ((t.at(end - 2) == QLatin1Char('-') && t.at(end - 1) == QLatin1Char('>')) ||
                     (t.at(end - 2) == QLatin1Char(':') && t.at(end - 1) == QLatin1Char(':'))))
        return 2;
    return 0;
}

// 1 for a plain identifier, 2 for one with a qualifier (`self.count`), 0 when `s` is not a name.
int nameSegments(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_$][\\w$]*(?:(?:\\.|::|->)[A-Za-z_$][\\w$]*)?$"));
    if (!re.match(s).hasMatch())
        return 0;
    return s.contains(QLatin1Char('.')) || s.contains(QLatin1String("::")) || s.contains(QLatin1String("->")) ? 2 : 1;
}

bool isOpener(QChar c) { return c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{'); }
bool isCloser(QChar c) { return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'); }
QChar closerOf(QChar c)
{
    return c == QLatin1Char('(') ? QLatin1Char(')') : c == QLatin1Char('[') ? QLatin1Char(']') : QLatin1Char('}');
}

// The partner of the bracket at `open`, skipping strings and comments; -1 when it is not closed.
int matchForward(const Scan &s, int open, int limit = 200000)
{
    QVector<QChar> stack;
    const int stop = qMin(s.t.size(), open + limit);
    for (int i = open; i < stop; ++i) {
        const QChar c = s.t.at(i);
        if (!isOpener(c) && !isCloser(c))
            continue;
        if (!s.code(i))
            continue;
        if (isOpener(c)) {
            stack.append(closerOf(c));
        } else {
            if (stack.isEmpty() || stack.last() != c)
                return -1;
            stack.removeLast();
            if (stack.isEmpty())
                return i;
        }
    }
    return -1;
}

// The unclosed '(' the position sits in, -1 when it is not inside parentheses of the same statement.
int enclosingParen(const Scan &s, int pos)
{
    int depth = 0;
    for (int i = pos - 1; i >= 0 && i >= pos - 3000; --i) {
        const QChar c = s.t.at(i);
        if (!isOpener(c) && !isCloser(c) && c != QLatin1Char(';'))
            continue;
        if (!s.code(i))
            continue;
        if (isCloser(c)) {
            if (c == QLatin1Char('}') && depth == 0)
                return -1;
            ++depth;
        } else if (isOpener(c)) {
            if (depth == 0)
                return c == QLatin1Char('(') ? i : -1;
            --depth;
        } else if (depth == 0) {
            return -1;
        }
    }
    return -1;
}

// Splits a parameter or argument list at its top-level commas. Ranges are trimmed and relative to `list`; a trailing
// empty piece (`a, b,` while typing) is dropped. `angles` also nests on <> (generic types in declarations).
struct Part {
    int start, end;
};
QVector<Part> splitList(const QString &list, bool angles)
{
    QVector<Part> parts;
    int depth = 0, from = 0;
    QChar quote;
    auto push = [&](int a, int b) {
        while (a < b && list.at(a).isSpace())
            ++a;
        while (b > a && list.at(b - 1).isSpace())
            --b;
        parts.append({a, b});
    };
    for (int i = 0; i < list.size(); ++i) {
        const QChar c = list.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\'))
                ++i;
            else if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`'))
            quote = c;
        else if (isOpener(c) || (angles && c == QLatin1Char('<')))
            ++depth;
        else if (isCloser(c) || (angles && c == QLatin1Char('>') && i > 0 && list.at(i - 1) != QLatin1Char('=') &&
                                 list.at(i - 1) != QLatin1Char('-')))
            depth = qMax(0, depth - 1);
        else if (c == QLatin1Char(',') && depth == 0) {
            push(from, i);
            from = i + 1;
        }
    }
    push(from, list.size());
    if (!parts.isEmpty() && parts.last().start == parts.last().end)
        parts.removeLast();
    return parts;
}

QStringList listTexts(const QString &list, bool angles)
{
    QStringList out;
    for (const Part &p : splitList(list, angles))
        out.append(list.mid(p.start, p.end - p.start).simplified());
    return out;
}

// Index of the top-level `ch` in a parameter, -1 for none (`::` and `==` / `=>` do not count).
int topLevel(const QString &p, QChar ch)
{
    int depth = 0;
    for (int i = 0; i < p.size(); ++i) {
        const QChar c = p.at(i);
        if (isOpener(c) || c == QLatin1Char('<'))
            ++depth;
        else if (isCloser(c) || c == QLatin1Char('>'))
            depth = qMax(0, depth - 1);
        else if (c == ch && depth == 0) {
            const QChar next = i + 1 < p.size() ? p.at(i + 1) : QChar();
            const QChar prev = i > 0 ? p.at(i - 1) : QChar();
            if (ch == QLatin1Char(':') && (next == QLatin1Char(':') || prev == QLatin1Char(':')))
                continue;
            if (ch == QLatin1Char('=') && (next == QLatin1Char('=') || next == QLatin1Char('>') || prev == QLatin1Char('=') ||
                                           prev == QLatin1Char('!') || prev == QLatin1Char('<') || prev == QLatin1Char('>')))
                continue;
            return i;
        }
    }
    return -1;
}

// A parameter callers may leave out: a default value, optional (`x?: T`), or variadic.
bool optionalParam(const QString &p)
{
    const QString s = p.trimmed();
    if (s.startsWith(QLatin1Char('*')) || s.startsWith(QLatin1String("...")) || s.contains(QLatin1String("...")) ||
        s.startsWith(QLatin1String("vararg ")) || s.startsWith(QLatin1String("params ")))
        return true;
    if (topLevel(s, QLatin1Char('=')) >= 0)
        return true;
    const int colon = topLevel(s, QLatin1Char(':'));
    return colon > 0 && s.at(colon - 1) == QLatin1Char('?');
}

// The name a parameter declares: `int depth` / `depth: number` / `final String depth = ""` -> depth.
QString paramName(const QString &param, const Context &ctx)
{
    QString p = param.trimmed();
    static const QRegularExpression annotation(QStringLiteral("^@[\\w.]+(?:\\([^)]*\\))?\\s*"));
    for (int guard = 0; guard < 4; ++guard) {
        const QRegularExpressionMatch m = annotation.match(p);
        if (!m.hasMatch())
            break;
        p = p.mid(m.capturedLength());
    }
    if (const int eq = topLevel(p, QLatin1Char('=')); eq >= 0)
        p = p.left(eq);
    static const QRegularExpression ident(QStringLiteral("[A-Za-z_$][\\w$]*"));
    QStringList words;
    const int colon = topLevel(p, QLatin1Char(':'));
    for (auto it = ident.globalMatch(colon >= 0 ? p.left(colon) : p); it.hasNext();)
        words.append(it.next().captured());
    static const QSet<QString> modifiers = {QStringLiteral("final"), QStringLiteral("const"), QStringLiteral("val"),
                                            QStringLiteral("var"), QStringLiteral("readonly"), QStringLiteral("public"),
                                            QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("mut"),
                                            QStringLiteral("ref"), QStringLiteral("out"), QStringLiteral("in"),
                                            QStringLiteral("inout"), QStringLiteral("override"), QStringLiteral("let")};
    while (!words.isEmpty() && modifiers.contains(words.first()))
        words.removeFirst();
    if (words.isEmpty())
        return {};
    if (colon >= 0)
        return words.first(); // `depth: number`, `depth: Int`
    if (ctx.language.isEmpty() && words.size() == 2)
        return words.first(); // Go: `depth int`
    return words.last();      // `int depth`, `String depth`, Python `depth`
}

const QSet<QString> &controlWords()
{
    static const QSet<QString> w = [] {
        QSet<QString> s;
        for (const char *k : {"if", "for", "while", "switch", "catch", "with", "return", "elif", "foreach", "sizeof", "typeof",
                              "super", "this", "new", "print", "assert", "await", "yield", "until", "unless", "match", "when",
                              "synchronized", "using", "lock", "fixed", "not", "and", "or", "in", "is", "else", "do", "try",
                              "throw", "delete", "defined", "decltype", "alignof", "static_assert", "noexcept", "require",
                              "import", "lambda", "function"})
            s.insert(QLatin1String(k));
        return s;
    }();
    return w;
}

// A function declaration (or definition): its name and the parentheses of its parameter list.
struct Decl {
    bool ok = false;
    QString name;
    int nameStart = -1, open = -1, close = -1;
    bool constructor = false; // `constructor(` / `__init__(`: called through the class name
};

QString classAbove(const QString &t, int pos)
{
    static const QRegularExpression re(QStringLiteral("\\bclass\\s+([A-Za-z_$][\\w$]*)"));
    QString name;
    for (auto it = re.globalMatch(t.left(pos)); it.hasNext();)
        name = it.next().captured(1);
    return name;
}

// Is the '(' at `open` the parameter list of a function declaration?
Decl declarationAt(const Scan &s, int open)
{
    const QString &t = s.t;
    Decl d;
    d.open = open;
    d.close = matchForward(s, open, 20000);
    if (d.close < 0)
        return d;
    const int ls = lineStart(t, open);
    int j = open;
    while (j > ls && (t.at(j - 1) == QLatin1Char(' ') || t.at(j - 1) == QLatin1Char('\t')))
        --j;
    if (j > ls && t.at(j - 1) == QLatin1Char('>')) { // function f<T>(
        int depth = 0;
        for (int k = j - 1; k >= ls; --k) {
            if (t.at(k) == QLatin1Char('>'))
                ++depth;
            else if (t.at(k) == QLatin1Char('<') && --depth == 0) {
                j = k;
                break;
            }
        }
        while (j > ls && t.at(j - 1).isSpace())
            --j;
    }
    const int we = j;
    while (j > ls && isIdent(t.at(j - 1)))
        --j;
    const QString word = t.mid(j, we - j);
    const QString prefix = t.mid(ls, j - ls);
    // What follows the parameter list, from the next non-space character on.
    int a = d.close + 1;
    while (a < t.size() && a < d.close + 400 && t.at(a).isSpace())
        ++a;
    const QString after = t.mid(a, 120);

    if (word.isEmpty() || word == QLatin1String("function")) {
        // Anonymous function bound to a name: `const f = (a) =>`, `f: function (a)`, `f = async (a) =>`
        static const QRegularExpression bound(QStringLiteral(
            "(?:^|[\\s,{(])(?:(?:export|const|let|var|static|readonly|public|private|protected)\\s+)*([A-Za-z_$][\\w$]*)\\s*(?::[^=]*)?[:=]\\s*(?:async\\s*)?(?:function\\s*\\*?\\s*)?$"));
        static const QRegularExpression arrowReturn(QStringLiteral("^:[^;{}]*?=>"));
        const QString before = t.mid(ls, we - ls);
        const QRegularExpressionMatch m = bound.match(before);
        const bool arrow = after.startsWith(QLatin1String("=>")) || arrowReturn.match(after).hasMatch();
        if (m.hasMatch() && (arrow || word == QLatin1String("function"))) {
            d.ok = true;
            d.name = m.captured(1);
            d.nameStart = ls + m.capturedStart(1);
        }
        return d;
    }
    if (controlWords().contains(word))
        return d;
    d.name = word;
    d.nameStart = j;

    static const QRegularExpression keyword(
        QStringLiteral("\\b(?:def|function|fun|fn|func|sub|proc)\\b\\*?[^=;(){}]*(?:\\([^)]*\\)[^=;(){}]*)?$"));
    if (keyword.match(prefix).hasMatch()) {
        d.ok = true;
    } else if (lang(s.ctx, {"JavaScript", "TypeScript"})) {
        // class method / object shorthand method: `async load(a) {`, `get size(): number {`
        static const QRegularExpression modifiers(QStringLiteral(
            "^\\s*(?:(?:public|private|protected|static|async|override|readonly|get|set|export|default|abstract|declare)\\s+)*\\*?$"));
        static const QRegularExpression typedBody(QStringLiteral("^:[^;{}=]*\\{"));
        d.ok = modifiers.match(prefix).hasMatch() && (after.startsWith(QLatin1Char('{')) || typedBody.match(after).hasMatch());
    } else if (lang(s.ctx, {"C", "C++", "Java"})) {
        QString pre = prefix;
        static const QRegularExpression qualifier(QStringLiteral("(?:[A-Za-z_][\\w]*(?:<[^;(){}]*>)?\\s*::\\s*)+$"));
        if (const QRegularExpressionMatch q = qualifier.match(pre); q.hasMatch())
            pre = pre.left(q.capturedStart()); // int Foo::bar(  /  Foo::Foo(
        static const QRegularExpression typed(QStringLiteral("^[\\w$:<>,\\[\\]*&\\s]*$"));
        static const QRegularExpression statement(
            QStringLiteral("^\\s*(?:return|new|throw|else|case|delete|await|yield|goto|co_return|co_await|using|typedef)\\b"));
        static const QRegularExpression tail(
            QStringLiteral("^(?:\\{|;|:[^:]|const\\b|override\\b|noexcept\\b|throws\\b|final\\b|->|=\\s*(?:0|default|delete)\\b|&|requires\\b)"));
        const QString trimmed = pre.trimmed();
        const bool typeOk = typed.match(pre).hasMatch() && !pre.contains(QLatin1String("<<")) && !statement.match(pre).hasMatch() &&
                            (trimmed.isEmpty() || isIdent(trimmed.back()) || trimmed.back() == QLatin1Char('>') ||
                             trimmed.back() == QLatin1Char('*') || trimmed.back() == QLatin1Char('&') ||
                             trimmed.back() == QLatin1Char(']'));
        const bool upperMacro = word == word.toUpper();
        if (typeOk && tail.match(after).hasMatch() && !upperMacro)
            d.ok = !trimmed.isEmpty() || after.startsWith(QLatin1Char('{')) || after.startsWith(QLatin1Char(':'));
    }
    if (!d.ok) {
        // Primary constructors and records: `class Point(val x: Int)`, `record Point(int x)`
        static const QRegularExpression typeDecl(QStringLiteral("\\b(?:class|struct|record)\\s+$"));
        if (typeDecl.match(prefix).hasMatch() && lang(s.ctx, {"Kotlin", "Java"}))
            d.ok = true;
    }
    if (d.ok && (word == QLatin1String("constructor") || word == QLatin1String("__init__"))) {
        const QString cls = classAbove(t, j);
        if (cls.isEmpty())
            d.ok = false;
        d.name = cls;
        d.constructor = true;
    }
    return d;
}

// [start, end) of a declared function: its line to the end of its body.
QPair<int, int> declScope(const Scan &s, const Decl &d)
{
    const QString &t = s.t;
    const int start = lineStart(t, d.nameStart >= 0 ? qMin(d.nameStart, d.open) : d.open);
    auto byIndent = [&]() -> QPair<int, int> {
        const int base = indentOf(t, start);
        int end = lineEnd(t, d.close);
        for (int ls = end + 1; ls < t.size();) {
            const int le = lineEnd(t, ls);
            if (!blankLine(t, ls)) {
                if (indentOf(t, ls) <= base)
                    break;
                end = le;
            }
            ls = le + 1;
        }
        return {start, end};
    };
    if (lang(s.ctx, {"Python"}))
        return byIndent();
    for (int i = d.close + 1; i < t.size() && i < d.close + 400; ++i) {
        const QChar c = t.at(i);
        if (!s.code(i))
            continue;
        if (c == QLatin1Char('{')) {
            const int e = matchForward(s, i);
            return {start, e < 0 ? t.size() : e + 1};
        }
        if (c == QLatin1Char(';'))
            return {start, i + 1}; // a prototype
        if (c == QLatin1Char('=') && i + 1 < t.size() && t.at(i + 1) == QLatin1Char('>')) {
            int k = i + 2;
            while (k < t.size() && t.at(k).isSpace())
                ++k;
            if (k < t.size() && t.at(k) == QLatin1Char('{')) {
                const int e = matchForward(s, k);
                return {start, e < 0 ? t.size() : e + 1};
            }
            return byIndent(); // expression body
        }
        if (c == QLatin1Char('=') || c == QLatin1Char(':'))
            return byIndent(); // Kotlin `= expr`, Python-like blocks
    }
    return byIndent();
}

struct Function {
    bool ok = false;
    Decl decl;
    QPair<int, int> scope;
};

// The innermost function whose declaration and body contain `pos`.
Function enclosingFunction(const Scan &s, int pos)
{
    const QString &t = s.t;
    int ls = lineStart(t, pos);
    for (int n = 0; n < kScanBack; ++n) {
        const int le = lineEnd(t, ls);
        for (int i = ls; i < le; ++i) {
            if (t.at(i) != QLatin1Char('(') || !s.code(i))
                continue;
            const Decl d = declarationAt(s, i);
            if (!d.ok)
                continue;
            const QPair<int, int> scope = declScope(s, d);
            if (scope.first <= pos && pos < scope.second)
                return {true, d, scope};
        }
        if (ls == 0)
            break;
        ls = lineStart(t, ls - 1);
    }
    return {};
}

bool paramsDeclare(const Scan &s, const Decl &d, const QString &name)
{
    const QString list = s.t.mid(d.open + 1, d.close - d.open - 1);
    for (const QString &p : listTexts(list, true))
        if (paramName(p, s.ctx) == name)
            return true;
    return false;
}

// Does the identifier at [a, b) declare a local variable (`let x`, `int x =`, `x := `, Python `x = `, `for x in`)?
bool declaresLocal(const Scan &s, int a, int b)
{
    const QString &t = s.t;
    const int ls = lineStart(t, a);
    const QString before = t.mid(ls, a - ls);
    const QString after = t.mid(b, 40);
    static const QRegularExpression binding(
        QStringLiteral("(?:^|[\\s(,;])(?:let|const|var|val|my|local|auto)\\s+(?:mut\\s+)?(?:[&*]\\s*)?$"));
    static const QRegularExpression walrus(QStringLiteral("^\\s*:="));
    static const QRegularExpression catchParam(QStringLiteral("\\bcatch\\s*\\(\\s*(?:[\\w.:<>]+\\s+)?$"));
    if (binding.match(before).hasMatch() || walrus.match(after).hasMatch() || catchParam.match(before).hasMatch())
        return true;
    if (lang(s.ctx, {"Python"})) {
        static const QRegularExpression assign(QStringLiteral("^\\s*(?:,[\\w\\s,]*)?(?::[^=]+)?=(?!=)"));
        static const QRegularExpression loop(QStringLiteral("\\b(?:for|async for)\\s+(?:[\\w\\s,(]*,\\s*)?\\(?$"));
        static const QRegularExpression as(QStringLiteral("\\bas\\s+$"));
        return (before.trimmed().isEmpty() && assign.match(after).hasMatch()) || loop.match(before).hasMatch() ||
               as.match(before).hasMatch();
    }
    if (lang(s.ctx, {"C", "C++", "Java"})) {
        static const QRegularExpression typed(QStringLiteral(
            "(?:^|[;({]\\s*)(?:(?:final|const|static|volatile|register|unsigned|signed|long|short)\\s+)*[A-Za-z_][\\w:<>,]*[\\s*&]+$"));
        static const QRegularExpression statement(QStringLiteral("^\\s*(?:return|else|case|goto|delete|new|throw)\\b"));
        static const QRegularExpression end(QStringLiteral("^\\s*(?:[=;,)\\[{]|:(?!:))"));
        return typed.match(before).hasMatch() && !statement.match(before).hasMatch() && end.match(after).hasMatch();
    }
    return false;
}

Prediction renamePrediction(const Scan &s, int coreStart, int coreEnd, const QString &oldCore)
{
    const QString &t = s.t;
    int ls = coreStart, re = coreEnd;
    while (ls > 0 && isIdent(t.at(ls - 1)))
        --ls;
    while (re < t.size() && isIdent(t.at(re)))
        ++re;
    if (const int op = memberOpBefore(t, ls); op > 0) { // keep one qualifier: `self.count`, not `count`
        int q = ls - op;
        while (q > 0 && isIdent(t.at(q - 1)))
            --q;
        if (q < ls - op)
            ls = q;
    }
    if (ls > coreStart || re < coreEnd)
        return {};
    const QString oldName = t.mid(ls, coreStart - ls) + oldCore + t.mid(coreEnd, re - coreEnd);
    const QString newName = t.mid(ls, re - ls);
    const int segments = nameSegments(oldName);
    if (oldName == newName || segments == 0 || segments != nameSegments(newName))
        return {};
    const bool qualified = segments == 2;

    // A function renamed at its declaration also changes where it is called as a member (`obj.area()`).
    bool functionName = false;
    {
        int k = re;
        while (k < t.size() && (t.at(k) == QLatin1Char(' ') || t.at(k) == QLatin1Char('\t')))
            ++k;
        if (!qualified && k < t.size() && t.at(k) == QLatin1Char('(')) {
            const Decl d = declarationAt(s, k);
            functionName = d.ok && d.nameStart == ls;
        }
    }

    // A parameter or local variable only changes inside its function.
    int from = 0, to = t.size();
    if (!qualified && !functionName) {
        const Function f = enclosingFunction(s, ls);
        if (f.ok) {
            bool local = (ls > f.decl.open && ls < f.decl.close) || paramsDeclare(s, f.decl, oldName) || declaresLocal(s, ls, re);
            for (int i = t.indexOf(oldName, f.scope.first); !local && i >= 0 && i < f.scope.second;
                 i = t.indexOf(oldName, i + 1)) {
                const int e = i + oldName.size();
                if ((i == 0 || !isIdent(t.at(i - 1))) && (e >= t.size() || !isIdent(t.at(e))) && memberOpBefore(t, i) == 0 &&
                    s.code(i))
                    local = declaresLocal(s, i, e);
            }
            if (local) {
                from = f.scope.first;
                to = f.scope.second;
            }
        }
    }

    Prediction p;
    p.kind = Kind::Rename;
    p.from = oldName;
    p.to = newName;
    for (int i = t.indexOf(oldName, from); i >= 0 && i + oldName.size() <= to; i = t.indexOf(oldName, i + 1)) {
        const int e = i + oldName.size();
        if (e > ls && i < re)
            continue; // the edited name itself
        if ((i > 0 && isIdent(t.at(i - 1))) || (e < t.size() && isIdent(t.at(e))))
            continue;
        if (memberOpBefore(t, i) > 0 && (qualified || !functionName))
            continue; // `user.name` is not the local `name`
        if (s.inString(i))
            continue;
        p.edits.append({i, e, newName});
        if (p.edits.size() >= kMaxEdits)
            break;
    }
    return p;
}

// A parameter added to or removed from a function declaration: the calls follow. `countChanged` tells the caller the
// edit changed the number of parameters (so it is not a rename).
Prediction parameterPrediction(const Scan &s, int coreStart, int coreEnd, const QString &oldCore, bool *countChanged)
{
    const QString &t = s.t;
    *countChanged = false;
    const int open = enclosingParen(s, coreStart);
    if (open < 0)
        return {};
    const Decl d = declarationAt(s, open);
    if (!d.ok || coreEnd > d.close)
        return {};
    const QString newList = t.mid(open + 1, d.close - open - 1);
    const QString oldList = t.mid(open + 1, coreStart - open - 1) + oldCore + t.mid(coreEnd, d.close - coreEnd);
    const QStringList np = listTexts(newList, true), op = listTexts(oldList, true);
    if (np.size() == op.size())
        return {};
    *countChanged = true;
    if (qAbs(np.size() - op.size()) != 1)
        return {};
    const bool added = np.size() > op.size();
    const QStringList &longer = added ? np : op;
    const QStringList &shorter = added ? op : np;
    int index = 0;
    while (index < shorter.size() && shorter.at(index) == longer.at(index))
        ++index;
    for (int k = index; k < shorter.size(); ++k)
        if (shorter.at(k) != longer.at(k + 1))
            return {}; // more changed than one parameter
    const QString param = longer.at(index);
    if (added && optionalParam(param))
        return {}; // callers may leave it out
    const QString name = paramName(param, s.ctx);
    if (name.isEmpty())
        return {};

    const bool python = lang(s.ctx, {"Python"});
    const QString first = op.isEmpty() ? QString() : paramName(op.first(), s.ctx);
    const int shift = python && (first == QLatin1String("self") || first == QLatin1String("cls")) ? 1 : 0;
    const int argIndex = index - shift;
    if (argIndex < 0)
        return {};
    const int oldCount = op.size() - shift;
    int required = 0;
    for (int k = shift; k < op.size(); ++k)
        if (!optionalParam(op.at(k)))
            ++required;

    Prediction p;
    p.kind = added ? Kind::AddArgument : Kind::RemoveArgument;
    p.from = d.name;
    p.to = name;
    static const QRegularExpression keywordArg(QStringLiteral("^[A-Za-z_][\\w]*\\s*=(?!=)"));
    for (int i = t.indexOf(d.name); i >= 0; i = t.indexOf(d.name, i + 1)) {
        const int e = i + d.name.size();
        if ((i > 0 && isIdent(t.at(i - 1))) || (e < t.size() && isIdent(t.at(e))) || i == d.nameStart || !s.code(i))
            continue;
        int k = e;
        while (k < t.size() && (t.at(k) == QLatin1Char(' ') || t.at(k) == QLatin1Char('\t')))
            ++k;
        if (k >= t.size() || t.at(k) != QLatin1Char('('))
            continue;
        if (declarationAt(s, k).ok)
            continue; // another declaration (an overload, a prototype)
        const bool member = memberOpBefore(t, i) > 0;
        if (d.constructor) {
            const QString lead = t.mid(lineStart(t, i), i - lineStart(t, i));
            if (!python && !lead.endsWith(QLatin1String("new ")))
                continue;
        } else if (shift > 0 && !member) {
            continue; // a method is called through an object
        }
        const int close = matchForward(s, k, 20000);
        if (close < 0)
            continue;
        const QString argText = t.mid(k + 1, close - k - 1);
        const QVector<Part> args = splitList(argText, false);
        bool unclear = false;
        for (const Part &a : args) {
            const QString at = argText.mid(a.start, a.end - a.start);
            if (keywordArg.match(at).hasMatch() || at.startsWith(QLatin1Char('*')) || at.startsWith(QLatin1String("...")))
                unclear = true; // keyword or spread arguments: positions say nothing
        }
        if (unclear || args.size() > oldCount || args.size() < required)
            continue; // another function with the same name, or one that takes other arguments
        const int base = k + 1;
        Edit edit;
        if (added) {
            if (args.size() < argIndex)
                continue;
            if (args.isEmpty()) {
                edit = {base, base, name, 0, int(name.size())};
            } else if (argIndex == 0) {
                const int at = base + args.first().start;
                edit = {at, at, name + QStringLiteral(", "), 0, int(name.size())};
            } else {
                const int at = base + args.at(argIndex - 1).end;
                edit = {at, at, QStringLiteral(", ") + name, 2, int(name.size())};
            }
        } else {
            if (args.size() <= argIndex)
                continue;
            if (argIndex + 1 < args.size())
                edit = {base + args.at(argIndex).start, base + args.at(argIndex + 1).start, QString()};
            else if (argIndex > 0)
                edit = {base + args.at(argIndex - 1).end, base + args.at(argIndex).end, QString()};
            else
                edit = {base + args.first().start, base + args.first().end, QString()};
        }
        p.edits.append(edit);
        if (p.edits.size() >= kMaxEdits)
            break;
    }
    return p;
}

} // namespace

Prediction predict(const QString &text, const Change &change, const Context &context)
{
    if (change.start < 0 || change.start + change.after.size() > text.size())
        return {};
    // Only the part that really changed: typing "count" over "counter" leaves "count" alone.
    int prefix = 0;
    const int shortest = qMin(change.before.size(), change.after.size());
    while (prefix < shortest && change.before.at(prefix) == change.after.at(prefix))
        ++prefix;
    int suffix = 0;
    while (suffix < shortest - prefix &&
           change.before.at(change.before.size() - 1 - suffix) == change.after.at(change.after.size() - 1 - suffix))
        ++suffix;
    const QString oldCore = change.before.mid(prefix, change.before.size() - prefix - suffix);
    const int coreStart = change.start + prefix;
    const int coreEnd = change.start + change.after.size() - suffix;
    if (oldCore.isEmpty() && coreEnd == coreStart)
        return {};

    const Scan s{text, context};
    // Edits inside strings and comments predict nothing.
    const int probe = coreEnd > coreStart ? coreStart : qMax(0, coreStart - 1);
    if (probe < text.size() && !s.code(probe))
        return {};

    bool countChanged = false;
    const Prediction params = parameterPrediction(s, coreStart, coreEnd, oldCore, &countChanged);
    if (countChanged)
        return params;
    return renamePrediction(s, coreStart, coreEnd, oldCore);
}

} // namespace EditPredict
