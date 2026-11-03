#include "CodeChunks.h"

#include "ai/AiEnv.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace agent::codeindex {

namespace {

constexpr int kTarget = 50;   // lines a section aims for
constexpr int kMax = 100;     // a section is cut at the best place once it gets this long
constexpr int kMin = 12;      // shorter tails merge into the section before
constexpr int kEmbedChars = 6000; // ~1.5k tokens; the model takes 8k

int indentOf(const QString &line)
{
    int n = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' '))
            ++n;
        else if (c == QLatin1Char('\t'))
            n += 4;
        else
            break;
    }
    return n;
}

bool isComment(const QString &trimmed)
{
    return trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1String("/*")) || trimmed.startsWith(QLatin1Char('*'))
        || (trimmed.startsWith(QLatin1Char('#')) && !trimmed.startsWith(QLatin1String("#include")) && !trimmed.startsWith(QLatin1String("#define")))
        || trimmed.startsWith(QLatin1String("--")) || trimmed.startsWith(QLatin1String("\"\"\"")) || trimmed.startsWith(QLatin1Char('@'))
        || trimmed.startsWith(QLatin1String("///"));
}

// Lines that continue the construct above rather than start a new one.
bool isContinuation(const QString &trimmed)
{
    static const QRegularExpression re(QStringLiteral(R"(^(\}|\)|\]|else\b|elif\b|except\b|catch\b|finally\b|#endif|#else|#elif|end\b|:|\.|\?|&&|\|\||\+|-[^-]))"));
    return re.match(trimmed).hasMatch();
}

const QRegularExpression &keywordDecl()
{
    // At the start of the line (after modifiers), so prose in comments and strings ("the function calls…") is not a declaration.
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*(?:(?:export|pub(?:\([\w:]+\))?|public|private|protected|internal|static|abstract|final|sealed|open|data|async|default|inline|unsafe|extern|override|partial|template\s*<[^>]*>)\s+)*)"
        R"((enum\s+(?:class|struct)|class|struct|interface|enum|trait|impl|namespace|module|object|record|protocol|extension|def|fn|func|function|fun|sub|proc|macro_rules!)\s+)"
        R"(([A-Za-z_$][\w$]*(?:(?:::|\.)[A-Za-z_$~][\w$]*)*))"));
    return re;
}

const QRegularExpression &cDecl()
{
    // "Type Name::method(" / "static int foo(" / "void Foo::~Foo(" at the start of a line, not a call statement.
    static const QRegularExpression re(QStringLiteral(R"(^\s*(?:[\w:<>,*&\[\]]+\s+)+[*&]*((?:[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\([^;]*$)"));
    return re;
}

const QRegularExpression &jsDecl()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*(?::[^=]+)?=\s*(?:async\s*)?(?:\([^)]*\)\s*(?::[^=]+)?=>|function\b|[A-Za-z_$][\w$]*\s*=>))"));
    return re;
}

const QRegularExpression &methodDecl()
{
    // JS/TS/Java/C# methods inside a class body: "  async load(id) {", "  public void run() {"
    static const QRegularExpression re(QStringLiteral(R"(^\s+(?:(?:public|private|protected|static|async|override|final|virtual|internal)\s+)*(?:[\w<>\[\],]+\s+)?([A-Za-z_$][\w$]*)\s*\([^;]*\)\s*(?::\s*[^{]+)?\{\s*$)"));
    return re;
}

bool notAName(const QString &name)
{
    static const QSet<QString> words = {"if", "for", "while", "switch", "return", "else", "catch", "new", "delete", "sizeof", "do", "case", "throw", "emit", "Q_EMIT", "foreach", "using", "typedef", "template"};
    return words.contains(name);
}

// The declared name on a line, or "".
QString declaredName(const QString &line, QString *kind)
{
    kind->clear();
    static const QRegularExpression statement(QStringLiteral(R"(^\s*(return|else|throw|case|emit|Q_EMIT|co_return|co_await|await|yield|new|delete|goto)\b)"));
    if (statement.match(line).hasMatch())
        return {};
    QRegularExpressionMatch m = keywordDecl().match(line);
    if (m.hasMatch()) {
        *kind = m.captured(1).simplified();
        return m.captured(2);
    }
    for (const QRegularExpression *re : {&jsDecl(), &cDecl(), &methodDecl()}) {
        m = re->match(line);
        if (m.hasMatch() && !notAName(m.captured(1)))
            return m.captured(1);
    }
    return {};
}

// How good a place line i (0-based) is to start a new section: 3 best, 0 never.
int cutScore(const QStringList &lines, int i, bool prose)
{
    const QString cur = lines.at(i).trimmed();
    if (prose && cur.startsWith(QLatin1Char('#')))
        return 3;
    if (cur.isEmpty() || isContinuation(cur))
        return 0;
    const QString prev = lines.at(i - 1).trimmed();
    const bool prevOpen = prev.isEmpty() || prev == QLatin1String("}") || prev == QLatin1String("};") || prev.endsWith(QLatin1Char(';'))
                          || prev == QLatin1String("end");
    const int indent = indentOf(lines.at(i));
    QString kind;
    const bool decl = !declaredName(lines.at(i), &kind).isEmpty();
    if (indent == 0 && (prev.isEmpty() || prev.startsWith(QLatin1Char('}')) || prev == QLatin1String("end")))
        return 3;
    if (decl && indent <= 4 && prevOpen)
        return 2;
    if (prev.isEmpty())
        return 1;
    return 0;
}

// Moves a cut up over the comment / decorator lines that belong to the declaration below it.
int withLeadingComments(const QStringList &lines, int cut, int floor)
{
    while (cut - 1 > floor) {
        const QString t = lines.at(cut - 1).trimmed();
        if (t.isEmpty() || !isComment(t))
            break;
        --cut;
    }
    return cut;
}

QString stem(QString w)
{
    auto cut = [&w](int n, const char *add = "") {
        w.chop(n);
        w += QLatin1String(add);
    };
    const int n = w.size();
    if (n > 7 && (w.endsWith(QLatin1String("ization")) || w.endsWith(QLatin1String("isation"))))
        cut(5);
    else if (n > 6 && w.endsWith(QLatin1String("ations")))
        cut(4);
    else if (n > 5 && w.endsWith(QLatin1String("ation")))
        cut(3);
    else if (n > 5 && w.endsWith(QLatin1String("ing")))
        cut(3);
    else if (n > 4 && w.endsWith(QLatin1String("ies")))
        cut(3, "y");
    else if (n > 4 && w.endsWith(QLatin1String("ed")))
        cut(2);
    else if (n > 4 && (w.endsWith(QLatin1String("ches")) || w.endsWith(QLatin1String("shes")) || w.endsWith(QLatin1String("xes"))
                       || w.endsWith(QLatin1String("sses"))))
        cut(2);
    else if (n > 3 && w.endsWith(QLatin1Char('s')) && !w.endsWith(QLatin1String("ss")) && !w.endsWith(QLatin1String("us"))
             && !w.endsWith(QLatin1String("is")))
        cut(1);
    if (w.size() > 5 && w.endsWith(QLatin1String("er")))
        w.chop(2);
    if (w.size() > 3 && w.endsWith(QLatin1Char('e')))
        w.chop(1);
    return w;
}

bool stopWord(const QString &w)
{
    static const QSet<QString> words = {
        // English
        "the", "an", "and", "or", "of", "to", "in", "is", "are", "be", "for", "on", "with", "as", "at", "by", "it", "this", "that", "from",
        "where", "what", "how", "which", "when", "who", "why", "does", "do", "we", "you", "before", "after", "into", "there", "all", "any",
        "not", "no", "can", "my", "our", "its", "if", "then", "than", "so", "but", "was", "were", "has", "have", "had", "will", "would",
        "should", "get", "set", "use", "used", "us", "about", "some", "find", "show", "me", "code", "thing", "way", "handle", "happen",
        // keywords every language repeats
        "else", "while", "return", "int", "void", "const", "static", "public", "private", "protected", "class", "struct", "new", "delete",
        "true", "false", "null", "nullptr", "none", "self", "auto", "var", "let", "def", "fn", "import", "include", "using", "namespace",
        "bool", "char", "double", "float", "long", "unsigned", "string", "qstring", "std", "qt", "elif", "end", "function", "func",
        "export", "default", "case", "break", "continue", "try", "catch", "throw", "async", "await", "lambda", "pass", "virtual",
        "override", "inline", "template", "typename", "typedef", "enum", "interface", "package", "val", "nil", "undefined", "typeof",
    };
    return words.contains(w);
}

void addWord(const QString &w, QStringList *out)
{
    if (w.size() < 2 || w.size() > 40 || w.at(0).isDigit())
        return;
    if (stopWord(w))
        return;
    const QString s = stem(w);
    if (s.size() >= 2 && !stopWord(s))
        out->append(s);
}

} // namespace

QVector<Section> split(const QStringList &lines, bool prose)
{
    QVector<Section> out;
    const int count = lines.size();
    if (count == 0)
        return out;
    auto push = [&](int start, int end) { // 0-based, end exclusive
        Section s;
        s.start = start + 1;
        s.end = end;
        out.append(s);
    };
    if (count <= kTarget * 13 / 10) {
        push(0, count);
    } else {
        int start = 0;
        for (int i = 1; i < count; ++i) {
            const int len = i - start;
            if (len < kMin)
                continue;
            if (len >= kTarget && cutScore(lines, i, prose) >= 2) {
                const int cut = withLeadingComments(lines, i, start + kMin);
                push(start, cut);
                start = cut;
                continue;
            }
            if (len >= kMax) {
                int best = -1, bestScore = 0;
                for (int j = i; j > start + kMin; --j) {
                    const int sc = cutScore(lines, j, prose);
                    if (sc > bestScore) {
                        best = j;
                        bestScore = sc;
                        if (sc == 3)
                            break;
                    }
                }
                const int cut = best > 0 ? withLeadingComments(lines, best, start + kMin) : i;
                push(start, cut);
                start = cut;
            }
        }
        if (start < count) {
            if (count - start < kMin && !out.isEmpty())
                out.last().end = count;
            else
                push(start, count);
        }
    }

    for (Section &s : out) {
        for (int i = s.start - 1; i < s.end && s.symbols.size() < 12; ++i) {
            const QString &line = lines.at(i);
            if (line.size() > 300 || (!prose && isComment(line.trimmed())))
                continue;
            if (prose) { // headings name the sections of a document
                if (line.startsWith(QLatin1Char('#'))) {
                    const QString heading = line.mid(line.indexOf(QLatin1Char(' ')) + 1).trimmed();
                    if (s.symbol.isEmpty())
                        s.symbol = heading;
                    s.symbols.append(heading);
                }
                continue;
            }
            QString kind;
            const QString name = declaredName(line, &kind);
            if (name.isEmpty() || s.symbols.contains(name))
                continue;
            if (s.symbol.isEmpty())
                s.symbol = kind.isEmpty() ? name : kind + QLatin1Char(' ') + name;
            s.symbols.append(name);
        }
    }
    return out;
}

QStringList terms(const QString &text)
{
    static const QRegularExpression ident(QStringLiteral(R"([A-Za-z_][A-Za-z0-9_]*)"));
    QStringList out;
    QRegularExpressionMatchIterator it = ident.globalMatch(text);
    while (it.hasNext()) {
        const QString id = it.next().captured(0);
        if (id.size() > 60)
            continue;
        const QString lower = id.toLower();
        // Parts: snake_case pieces, then camelCase / HTTPServer / v2 boundaries inside each.
        QStringList parts;
        for (const QString &piece : id.split(QLatin1Char('_'), Qt::SkipEmptyParts)) {
            int from = 0;
            for (int i = 1; i < piece.size(); ++i) {
                const QChar a = piece.at(i - 1), b = piece.at(i);
                const bool lowerUpper = a.isLower() && b.isUpper();
                const bool acronymEnd = a.isUpper() && b.isUpper() && i + 1 < piece.size() && piece.at(i + 1).isLower();
                const bool digit = a.isDigit() != b.isDigit();
                if (lowerUpper || acronymEnd || digit) {
                    parts << piece.mid(from, i - from).toLower();
                    from = i;
                }
            }
            parts << piece.mid(from).toLower();
        }
        if (parts.size() > 1)
            addWord(lower, &out);
        for (const QString &p : std::as_const(parts))
            addWord(p, &out);
    }
    return out;
}

quint32 termHash(const QString &term)
{
    quint32 h = 2166136261u; // FNV-1a over the UTF-16 units
    for (const QChar c : term) {
        h ^= c.unicode();
        h *= 16777619u;
    }
    return h;
}

QString embedText(const QString &rel, const Section &section, const QStringList &lines)
{
    QString title = rel;
    if (!section.symbol.isEmpty())
        title += QLatin1Char(' ') + section.symbol;
    QString body;
    for (int i = section.start - 1; i < section.end && i < lines.size(); ++i) {
        body += lines.at(i).left(400);
        body += QLatin1Char('\n');
        if (body.size() > kEmbedChars)
            break;
    }
    return QStringLiteral("title: %1 | text: %2").arg(title, body.left(kEmbedChars));
}

QByteArray keyOf(const QString &embedText)
{
    return QCryptographicHash::hash(embedText.toUtf8(), QCryptographicHash::Sha1).left(16);
}

bool isProse(const QString &rel)
{
    const QString suffix = QFileInfo(rel).suffix().toLower();
    return suffix == QLatin1String("md") || suffix == QLatin1String("markdown") || suffix == QLatin1String("rst") || suffix == QLatin1String("txt")
        || suffix == QLatin1String("adoc");
}

bool indexableName(const QString &rel)
{
    if (AiEnv::isSensitive(rel))
        return false;
    static const QSet<QString> skipDirs = {".git", "node_modules", "build", "dist", "out", ".gradle", "__pycache__", ".venv", "venv", ".cache",
                                           ".idea", ".next", ".nuxt", "target", ".dart_tool", "Pods", ".mypy_cache", ".pytest_cache", "vendor",
                                           "third_party", ".svn", ".hg", "coverage", ".expo"};
    const QStringList parts = rel.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < parts.size(); ++i)
        if (skipDirs.contains(parts.at(i)))
            return false;
    const QString name = parts.last().toLower();
    static const QSet<QString> skipNames = {"package-lock.json", "yarn.lock", "pnpm-lock.yaml", "cargo.lock", "poetry.lock", "composer.lock",
                                            "gemfile.lock", "go.sum", "bun.lockb", "uv.lock", "pipfile.lock"};
    if (skipNames.contains(name) || name.endsWith(QLatin1String(".min.js")) || name.endsWith(QLatin1String(".min.css"))
        || name.endsWith(QLatin1String(".map")) || name.endsWith(QLatin1String(".bundle.js")))
        return false;
    static const QSet<QString> bin = {"png", "jpg", "jpeg", "gif", "webp", "ico", "icns", "pdf", "zip", "gz", "xz", "bz2", "7z", "tar", "jar",
                                      "war", "ttf", "otf", "woff", "woff2", "eot", "mp3", "mp4", "mov", "webm", "wav", "ogg", "flac", "so",
                                      "a", "o", "obj", "dll", "dylib", "class", "pyc", "deb", "rpm", "exe", "bin", "lock", "sqlite", "db",
                                      "bmp", "tif", "tiff", "psd", "svg", "keystore", "jks", "p12", "wasm", "dat", "pak", "ipa", "apk", "aab"};
    return !bin.contains(QFileInfo(name).suffix());
}

} // namespace agent::codeindex
