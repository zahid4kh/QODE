#include "ShellSyntax.h"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QRegularExpression>

#include <memory>

namespace {

using ShellSyntax::Dialect;

// --- sh, the part QODE writes -------------------------------------------------------------------------------------

struct Word {
    QString value;      // with the quotes taken off
    bool quoted = false; // some of it was quoted
    QString assignName; // NAME=value with an unquoted NAME: the name and the value
    QString assignValue;
};

enum class Tok { Word, And, Or, Semi, Pipe, Open, Close };

struct Token {
    Tok kind;
    Word word;
};

bool isName(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return re.match(s).hasMatch();
}

// False for anything outside the subset. A backslash only escapes a quote, a space or a shell character after it:
// on Windows a line can also hold .\gradlew, whose backslash is part of the word.
bool tokenize(const QString &line, QList<Token> *out)
{
    const int n = int(line.size());
    int i = 0;
    while (i < n) {
        const QChar c = line.at(i);
        if (c.isSpace()) {
            ++i;
            continue;
        }
        const QString two = line.mid(i, 2);
        if (two == QLatin1String("&&") || two == QLatin1String("||")) {
            out->append({two == QLatin1String("&&") ? Tok::And : Tok::Or, {}});
            i += 2;
            continue;
        }
        if (c == QLatin1Char(';') || c == QLatin1Char('|') || c == QLatin1Char('(') || c == QLatin1Char(')')) {
            out->append({c == QLatin1Char(';') ? Tok::Semi : c == QLatin1Char('|') ? Tok::Pipe : c == QLatin1Char('(') ? Tok::Open : Tok::Close, {}});
            ++i;
            continue;
        }
        if (c == QLatin1Char('#') || c == QLatin1Char('~'))
            return false; // a comment, the home folder
        Word w;
        int eq = -1;
        while (i < n) {
            const QChar ch = line.at(i);
            if (ch.isSpace() || ch == QLatin1Char(';') || ch == QLatin1Char('|') || ch == QLatin1Char('(') || ch == QLatin1Char(')'))
                break;
            if (ch == QLatin1Char('&')) {
                if (i + 1 < n && line.at(i + 1) == QLatin1Char('&'))
                    break;
                return false; // in the background
            }
            if (ch == QLatin1Char('\'')) {
                const int end = int(line.indexOf(QLatin1Char('\''), i + 1));
                if (end < 0)
                    return false;
                w.value += line.mid(i + 1, end - i - 1);
                w.quoted = true;
                i = end + 1;
                continue;
            }
            if (ch == QLatin1Char('"')) {
                bool closed = false;
                for (++i; i < n; ++i) {
                    const QChar d = line.at(i);
                    if (d == QLatin1Char('"')) {
                        closed = true;
                        ++i;
                        break;
                    }
                    if (d == QLatin1Char('$') || d == QLatin1Char('`'))
                        return false;
                    if (d == QLatin1Char('\\') && i + 1 < n && QStringLiteral("\"\\$`").contains(line.at(i + 1)))
                        ++i;
                    w.value += line.at(i);
                }
                if (!closed)
                    return false;
                w.quoted = true;
                continue;
            }
            if (ch == QLatin1Char('\\') && i + 1 < n && QStringLiteral("'\" $`();&|").contains(line.at(i + 1))) {
                w.value += line.at(i + 1);
                w.quoted = true;
                i += 2;
                continue;
            }
            if (QStringLiteral("$`<>*?{}").contains(ch))
                return false; // expansions, redirections, globs
            if (ch == QLatin1Char('=') && eq < 0 && !w.quoted)
                eq = int(w.value.size());
            w.value += ch;
            ++i;
        }
        if (eq > 0 && isName(w.value.left(eq))) {
            w.assignName = w.value.left(eq);
            w.assignValue = w.value.mid(eq + 1);
        }
        out->append({Tok::Word, w});
    }
    return true;
}

struct List;

struct Simple {
    QList<QPair<QString, QString>> env; // VAR=value before the command
    QList<Word> words;
};

struct Command {
    Simple simple;
    std::shared_ptr<List> group; // ( … )
};

struct Pipeline {
    QList<Command> commands;
};

struct AndOr {
    QList<Pipeline> pipelines;
    QList<Tok> ops; // And / Or between them
};

struct List {
    QList<AndOr> items; // separated by ;
};

class Parser
{
public:
    explicit Parser(const QList<Token> &tokens)
        : m_t(tokens)
    {
    }

    bool parse(List *out) { return list(out) && m_i == m_t.size(); }

private:
    bool at(Tok kind) const { return m_i < m_t.size() && m_t.at(m_i).kind == kind; }

    bool list(List *out)
    {
        for (;;) {
            AndOr a;
            if (!andOr(&a))
                return false;
            out->items.append(a);
            if (!at(Tok::Semi))
                return true;
            ++m_i;
            if (m_i == m_t.size() || at(Tok::Close)) // a trailing ;
                return true;
        }
    }

    bool andOr(AndOr *out)
    {
        Pipeline p;
        if (!pipeline(&p))
            return false;
        out->pipelines.append(p);
        while (at(Tok::And) || at(Tok::Or)) {
            out->ops.append(m_t.at(m_i++).kind);
            Pipeline q;
            if (!pipeline(&q))
                return false;
            out->pipelines.append(q);
        }
        return true;
    }

    bool pipeline(Pipeline *out)
    {
        Command c;
        if (!command(&c))
            return false;
        out->commands.append(c);
        while (at(Tok::Pipe)) {
            ++m_i;
            Command d;
            if (!command(&d))
                return false;
            out->commands.append(d);
        }
        return true;
    }

    bool command(Command *out)
    {
        if (at(Tok::Open)) {
            ++m_i;
            out->group = std::make_shared<List>();
            if (!list(out->group.get()) || !at(Tok::Close))
                return false;
            ++m_i;
            return true;
        }
        while (at(Tok::Word) && !m_t.at(m_i).word.assignName.isEmpty()) {
            const Word &w = m_t.at(m_i++).word;
            out->simple.env.append({w.assignName, w.assignValue});
        }
        while (at(Tok::Word))
            out->simple.words.append(m_t.at(m_i++).word);
        return !out->simple.words.isEmpty(); // a bare VAR=value (a shell variable) is not in the subset
    }

    const QList<Token> &m_t;
    qsizetype m_i = 0;
};

// The whole line as one ( … ): its inside, else null. A ( … ) anywhere else is not in the subset.
const List *wholeGroup(const List &l)
{
    if (l.items.size() == 1 && l.items.first().pipelines.size() == 1 && l.items.first().pipelines.first().commands.size() == 1)
        return l.items.first().pipelines.first().commands.first().group.get();
    return nullptr;
}

// --- writing it out -------------------------------------------------------------------------------------------------

QString psQuote(const QString &s, bool always = false)
{
    // Bare words PowerShell passes as they are. A word starting with '-' that holds a '.' must be quoted all the same:
    // Windows PowerShell splits -Dname.part=x at the '.'.
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_./\\\\:+=%-]+$"));
    if (!always && safe.match(s).hasMatch() && !(s.startsWith(QLatin1Char('-')) && s.contains(QLatin1Char('.'))))
        return s;
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString cmdQuote(const QString &s)
{
    static const QString special = QStringLiteral(" \t&|<>^(),;=%!\"");
    bool needs = s.isEmpty();
    for (const QChar c : s)
        needs = needs || special.contains(c);
    if (!needs)
        return s;
    QString q = s;
    q.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

QString shQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

class Writer
{
public:
    explicit Writer(Dialect d)
        : m_d(d)
    {
    }

    // False when the line holds something this cannot write (a ( … ) inside the line).
    bool write(const List &line, QString *out)
    {
        const List *inner = wholeGroup(line);
        m_subshell = inner != nullptr;
        const QString body = list(inner ? *inner : line);
        if (!m_ok)
            return false;
        if (m_env.isEmpty() && !m_subshell) {
            *out = body;
            return true;
        }
        *out = m_d == Dialect::PowerShell ? wrapPowerShell(body) : wrapCmd(body);
        return true;
    }

private:
    // Saved, set for the try, restored in finally; the result goes to $__qodeOk (see the header).
    QString wrapPowerShell(const QString &body) const
    {
        QString pre = QStringLiteral("$__qodeOk = $false; ");
        QString set, restore;
        if (m_subshell)
            pre += QStringLiteral("Push-Location; ");
        for (int i = 0; i < m_env.size(); ++i) {
            const QString &name = m_env.at(i).first;
            pre += QStringLiteral("$__qodeEnv%1 = $env:%2; ").arg(i).arg(name);
            set += QStringLiteral("$env:%1 = %2; ").arg(name, psQuote(m_env.at(i).second, true));
            restore += QStringLiteral("$env:%1 = $__qodeEnv%2; ").arg(name).arg(i);
        }
        if (m_subshell)
            restore += QStringLiteral("Pop-Location; ");
        restore.chop(2);
        return pre + QStringLiteral("try { ") + set + body + QStringLiteral("; $__qodeOk = $? } finally { ") + restore + QStringLiteral(" }");
    }

    // A child cmd: what it sets and where it goes end with it.
    QString wrapCmd(const QString &body) const
    {
        QString inner;
        for (const auto &e : m_env)
            inner += QStringLiteral("set \"%1=%2\" && ").arg(e.first, e.second);
        return QStringLiteral("cmd /d /s /c \"") + inner + body + QLatin1Char('"');
    }

    QString list(const List &l)
    {
        QStringList parts;
        for (const AndOr &a : l.items)
            parts << andOr(a, 0);
        return parts.join(m_d == Dialect::Cmd ? QStringLiteral(" & ") : QStringLiteral("; "));
    }

    QString andOr(const AndOr &a, int i)
    {
        QString s = pipeline(a.pipelines.at(i));
        if (i + 1 >= a.pipelines.size())
            return s;
        const bool isAnd = a.ops.at(i) == Tok::And;
        if (m_d == Dialect::Cmd)
            return s + (isAnd ? QStringLiteral(" && ") : QStringLiteral(" || ")) + andOr(a, i + 1);
        // Windows PowerShell has no && / ||; $? after a native command is false when its exit code was not 0.
        return s + (isAnd ? QStringLiteral("; if ($?) { ") : QStringLiteral("; if (-not $?) { ")) + andOr(a, i + 1) + QStringLiteral(" }");
    }

    QString pipeline(const Pipeline &p)
    {
        QStringList parts;
        for (const Command &c : p.commands)
            parts << command(c);
        return parts.join(QStringLiteral(" | "));
    }

    QString command(const Command &c)
    {
        if (c.group) {
            m_ok = false;
            return {};
        }
        // VAR=value applies to the whole line here, not to its command alone: enough for what QODE writes.
        m_env += c.simple.env;
        const QList<Word> &w = c.simple.words;
        const bool ps = m_d == Dialect::PowerShell;
        if (w.first().value == QLatin1String("cd") && !w.first().quoted && w.size() <= 2) {
            if (w.size() == 1)
                return ps ? QStringLiteral("Set-Location ~") : QStringLiteral("cd /d %USERPROFILE%");
            return ps ? QStringLiteral("Set-Location -LiteralPath ") + psQuote(w.at(1).value, true)
                      : QStringLiteral("cd /d ") + cmdQuote(QDir::toNativeSeparators(w.at(1).value));
        }
        if (w.first().value == QLatin1String("mkdir") && !w.first().quoted && w.size() >= 3 && w.at(1).value == QLatin1String("-p")) {
            QStringList dirs;
            for (int i = 2; i < w.size(); ++i) {
                if (ps) {
                    dirs << psQuote(w.at(i).value, true);
                } else {
                    const QString d = QDir::toNativeSeparators(w.at(i).value);
                    dirs << QStringLiteral("(if not exist \"%1\\\" md \"%1\")").arg(d);
                }
            }
            // An existing folder is no error, as with -p.
            return ps ? QStringLiteral("$null = New-Item -ItemType Directory -Force -Path ") + dirs.join(QLatin1Char(','))
                      : dirs.join(QStringLiteral(" && "));
        }
        QString out;
        for (int i = 0; i < w.size(); ++i) {
            QString word = w.at(i).value;
            if (i == 0 && !ps && word.contains(QLatin1Char('/')))
                word = QDir::toNativeSeparators(word); // cmd: ./gradlew is the command '.' with a switch
            const QString q = ps ? psQuote(word) : cmdQuote(word);
            if (i == 0 && ps && q.startsWith(QLatin1Char('\'')))
                out += QStringLiteral("& "); // a quoted program is a string to PowerShell until it is called
            else if (i > 0)
                out += QLatin1Char(' ');
            out += q;
        }
        return out;
    }

    Dialect m_d;
    bool m_ok = true;
    bool m_subshell = false;
    QList<QPair<QString, QString>> m_env;
};

} // namespace

namespace ShellSyntax {

Dialect dialectOf(const QString &shell)
{
    QString name = QFileInfo(shell).fileName().toLower();
    const bool exe = name.endsWith(QLatin1String(".exe"));
    if (exe)
        name.chop(4);
    if (name == QLatin1String("pwsh") || name == QLatin1String("powershell"))
        return Dialect::PowerShell;
    if (name == QLatin1String("cmd"))
        return Dialect::Cmd;
    if (name == QLatin1String("wsl"))
        return Dialect::Wsl;
    if (exe)
        return Dialect::GitBash; // bash.exe, sh.exe, zsh.exe: an MSYS / Cygwin shell
    return Dialect::Posix;
}

QString translate(const QString &line, const QString &shell)
{
    const Dialect d = dialectOf(shell);
    if (d == Dialect::Posix)
        return line;
    if (d == Dialect::GitBash || d == Dialect::Wsl) {
        // .\gradlew.bat / .\mvnw.cmd -> ./gradlew / ./mvnw (the sh scripts next to them), any other .\x -> ./x.
        static const QRegularExpression script(QStringLiteral("(^|[\\s;&|(])\\.\\\\(gradlew|mvnw)\\.(?:bat|cmd)(?=\\s|$|[;&|)])"),
                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression wrapper(QStringLiteral("(^|[\\s;&|(])\\.\\\\(?=\\w)"));
        QString out = line;
        out.replace(script, QStringLiteral("\\1./\\2"));
        out.replace(wrapper, QStringLiteral("\\1./"));
        if (d == Dialect::Wsl) {
            // A drive path at the start of a word (also inside quotes, or after NAME=): C:/x -> /mnt/c/x.
            static const QRegularExpression drive(QStringLiteral("(^|[\\s'\"=(:])([A-Za-z]):[/\\\\]"));
            QString mapped;
            qsizetype last = 0;
            for (auto it = drive.globalMatch(out); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                mapped += out.mid(last, m.capturedStart(2) - last) + QStringLiteral("/mnt/") + m.captured(2).toLower() + QLatin1Char('/');
                last = m.capturedEnd(0);
            }
            out = mapped + out.mid(last);
        }
        return out;
    }
    QList<Token> tokens;
    List list;
    if (!tokenize(line, &tokens) || tokens.isEmpty() || !Parser(tokens).parse(&list))
        return line;
    QString out;
    return Writer(d).write(list, &out) ? out : line;
}

QString quote(Dialect dialect, const QString &word)
{
    switch (dialect) {
    case Dialect::PowerShell: return psQuote(word, true);
    case Dialect::Cmd: return cmdQuote(word);
    default: return shQuote(word);
    }
}

QString sequence(Dialect dialect, const QStringList &commands)
{
    return commands.join(dialect == Dialect::Cmd ? QStringLiteral(" & ") : QStringLiteral("; "));
}

} // namespace ShellSyntax
