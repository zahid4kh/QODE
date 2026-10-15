#include "AgentTools.h"

#include "ai/AiEnv.h"
#include "palette/FuzzyMatcher.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <csignal>
#include <unistd.h>

namespace agent {

namespace {

constexpr int kMaxResponseChars = 24000; // everything returned is resent with every later request
constexpr int kReadChars = 16000;
constexpr int kDefaultReadLines = 400;

struct Param {
    const char *name;
    const char *type; // string | integer | boolean | array (of strings)
    const char *description;
    bool required = true;
};

QJsonObject makeSchema(std::initializer_list<Param> params)
{
    QJsonObject props;
    QJsonArray required;
    for (const Param &p : params) {
        QJsonObject prop{{"type", QString::fromLatin1(p.type)}, {"description", QString::fromLatin1(p.description)}};
        if (QLatin1String(p.type) == QLatin1String("array"))
            prop.insert("items", QJsonObject{{"type", "string"}});
        props.insert(QString::fromLatin1(p.name), prop);
        if (p.required)
            required.append(QString::fromLatin1(p.name));
    }
    QJsonObject schema{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty())
        schema.insert("required", required);
    return schema;
}

ToolResult fail(const QString &message)
{
    ToolResult r;
    r.ok = false;
    r.response = QJsonObject{{"error", message}};
    r.summary = message;
    return r;
}

ToolResult success(const QJsonObject &response, const QString &summary)
{
    ToolResult r;
    r.response = response;
    r.summary = summary;
    return r;
}

QString str(const QJsonObject &a, const char *key)
{
    return a.value(QLatin1String(key)).toString();
}

QString shorten(const QString &s, int n)
{
    const QString one = s.simplified();
    return one.size() > n ? one.left(n - 1) + QChar(0x2026) : one;
}

QString plural(int n, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
}

// Folders no tool walks into: dependencies, build output, caches, version control.
bool ignoredDir(const QString &name)
{
    static const QSet<QString> skip = {".git", "node_modules", "build", "dist", "out", ".gradle", "__pycache__", ".venv", "venv", ".cache",
                                       ".idea", ".next", ".nuxt", "target", ".dart_tool", "Pods", ".mypy_cache", ".pytest_cache", ".svn", ".hg"};
    return skip.contains(name);
}

bool binarySuffix(const QString &name)
{
    static const QSet<QString> bin = {"png", "jpg", "jpeg", "gif", "webp", "ico", "pdf", "zip", "gz", "xz", "jar", "ttf", "otf", "woff", "woff2",
                                      "mp3", "mp4", "wav", "ogg", "so", "a", "o", "class", "deb", "exe", "bin", "lock", "sqlite", "db", "bmp", "tar"};
    return bin.contains(QFileInfo(name).suffix().toLower());
}

// The lines of a text the way the editor counts them: a final newline does not start another line.
struct Lines {
    explicit Lines(const QString &t)
        : text(t)
    {
        starts.append(0);
        for (int i = 0; i < t.size(); ++i)
            if (t.at(i) == QLatin1Char('\n'))
                starts.append(i + 1);
        count = t.isEmpty() ? 0 : (t.endsWith(QLatin1Char('\n')) ? starts.size() - 1 : starts.size());
    }
    int lineOf(int offset) const // 1-based
    {
        const int n = int(std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin());
        return qBound(1, n, qMax(1, count));
    }
    int lineStart(int line) const { return starts.at(qBound(1, line, starts.size()) - 1); }
    int lineEnd(int line) const // exclusive, without the line break
    {
        return line < starts.size() ? starts.at(line) - 1 : text.size();
    }
    QString line(int n) const { return text.mid(lineStart(n), lineEnd(n) - lineStart(n)); }

    QString text;
    QVector<int> starts;
    int count = 0;
};

// "  12 | text" for lines first..last (1-based, inclusive).
QString numberedRange(const Lines &L, int first, int last, int *stoppedAt = nullptr, int maxChars = kReadChars)
{
    const int width = QString::number(qMax(1, L.count)).size();
    QString out;
    int n = first;
    for (; n <= last && n <= L.count; ++n) {
        const QString row = QStringLiteral("%1 | %2\n").arg(n, width).arg(L.line(n));
        if (out.size() + row.size() > maxChars && n > first)
            break;
        out += row;
    }
    if (stoppedAt)
        *stoppedAt = n; // first line NOT included
    return out;
}

QString clipTail(const QByteArray &data, int limit = 8000)
{
    QString text = QString::fromUtf8(data);
    if (data.size() > limit)
        text = QStringLiteral("...(earlier output cut)...\n") + QString::fromUtf8(data.right(limit));
    return text;
}

// Rejects the commands no coding task needs: privilege escalation, disk tools, power commands, piping downloads into a shell.
QString commandDenied(const QString &command)
{
    struct Rule {
        const char *pattern, *reason;
    };
    static const Rule rules[] = {
        {R"(\bsudo\b|\bsu\s|\bpkexec\b|\bdoas\b)", "privileged commands (sudo/su/pkexec) are not allowed"},
        {R"(\brm\b[^|;&]*\s-[a-zA-Z]*[rR][a-zA-Z]*[^|;&]*\s(/|~|\$HOME|/\*|~/\*|\.\.?)(\s|$))", "recursive delete of /, home or the project root is not allowed"},
        {R"(\bmkfs(\.\w+)?\b|\bwipefs\b|\bfdisk\b|\bparted\b)", "disk formatting is not allowed"},
        {R"(\bdd\b[^|;&]*\bof=/dev/)", "writing to block devices is not allowed"},
        {R"(\b(shutdown|reboot|poweroff|halt)\b|systemctl\s+(poweroff|reboot|halt))", "power commands are not allowed"},
        {R"(:\(\)\s*\{)", "fork bombs are not allowed"},
        {R"((curl|wget)\b[^|;&]*\|\s*(sudo\s+)?(ba|z)?sh\b)", "piping downloads into a shell is not allowed"},
        {R"(\.ssh\b|\.gnupg\b|\.aws\b)", "commands touching credential locations are not allowed"},
        {R"(\bgit\s+push\b)", "pushing is left to the user"},
    };
    for (const Rule &r : rules)
        if (QRegularExpression(QString::fromLatin1(r.pattern)).match(command).hasMatch())
            return QString::fromLatin1(r.reason);
    return {};
}

} // namespace

// --- setup ----------------------------------------------------------------------------------------------------------

void AgentTools::setRoot(const QString &root)
{
    m_root = QDir::cleanPath(root);
    m_touched.clear();
    m_created.clear();
    m_gone.clear();
    if (m_specs.isEmpty())
        registerAll();
}

QJsonArray AgentTools::toolsJson() const
{
    QJsonArray decls;
    for (const Spec &s : m_specs) {
        QJsonObject d{{"name", s.name}, {"description", s.description}};
        if (!s.parameters.value("properties").toObject().isEmpty())
            d.insert("parameters", s.parameters); // Gemini rejects an OBJECT schema with empty properties
        decls.append(d);
    }
    return QJsonArray{QJsonObject{{"functionDeclarations", decls}}};
}

void AgentTools::cancel()
{
    m_cancelled = true;
}

QString AgentTools::describe(const QString &name, const QJsonObject &args) const
{
    for (const Spec &s : m_specs)
        if (s.name == name)
            return s.call ? s.call(args) : name;
    return name;
}

ToolResult AgentTools::execute(const QString &name, const QJsonObject &args)
{
    const Spec *spec = nullptr;
    for (const Spec &s : std::as_const(m_specs))
        if (s.name == name)
            spec = &s;
    if (!spec) {
        ToolResult r = fail(QStringLiteral("unknown tool: %1").arg(name));
        r.call = name;
        return r;
    }
    const QString call = spec->call ? spec->call(args) : name;
    if (m_cancelled) {
        ToolResult r = fail(QStringLiteral("cancelled by the user"));
        r.call = call;
        return r;
    }
    if (spec->risk == Risk::Dangerous) {
        const QString detail = spec->detail ? spec->detail(args) : QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented));
        if (!m_hooks.approve || !m_hooks.approve(name, call, detail)) {
            ToolResult r = fail(QStringLiteral("the user denied this action; do not retry it, say so and continue without it"));
            r.call = call;
            r.summary = QStringLiteral("denied");
            return r;
        }
    }
    ToolResult r = spec->run(args);
    r.call = call;
    if (QJsonDocument(r.response).toJson(QJsonDocument::Compact).size() > kMaxResponseChars) {
        const bool wasOk = r.ok;
        r.response = QJsonObject{{"error", "result too large; narrow the path or pattern, or read a smaller range"}};
        r.ok = false;
        if (wasOk)
            r.summary = QStringLiteral("result too large");
    }
    return r;
}

// --- helpers --------------------------------------------------------------------------------------------------------

bool AgentTools::resolve(const QString &input, QString *absolute, QString *error, bool forWrite) const
{
    QString p = input.trimmed();
    if (p.isEmpty())
        p = QStringLiteral(".");
    if (p.startsWith(QLatin1Char('~'))) {
        *error = QStringLiteral("use paths relative to the project root");
        return false;
    }
    const QString full = QDir::cleanPath(QDir::isAbsolutePath(p) ? p : m_root + QLatin1Char('/') + p);

    // Judge a not-yet-existing path by where it would really land (through symlinks of its existing part).
    QString tail;
    QFileInfo info(full);
    while (!info.exists() && !info.isSymLink() && info.absoluteFilePath() != QLatin1String("/")) {
        tail = QLatin1Char('/') + info.fileName() + tail;
        info = QFileInfo(info.absolutePath());
    }
    const QString canonRoot = QFileInfo(m_root).canonicalFilePath();
    const QString canon = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    const QString real = QDir::cleanPath(canon + tail);
    const QString base = canonRoot.isEmpty() ? m_root : canonRoot;
    if (real != base && !real.startsWith(base + QLatin1Char('/'))) {
        *error = QStringLiteral("'%1' is outside the project").arg(input);
        return false;
    }
    const QString inside = real.mid(base.size()); // "" or "/a/b"
    *absolute = m_root + inside;
    if (AiEnv::isSensitive(*absolute)) {
        *error = QStringLiteral("'%1' looks like a secret or credential file and is off limits").arg(QFileInfo(*absolute).fileName());
        return false;
    }
    if (forWrite) {
        if (inside.isEmpty()) {
            *error = QStringLiteral("the project root itself cannot be changed");
            return false;
        }
        for (const QString &part : inside.split(QLatin1Char('/'), Qt::SkipEmptyParts))
            if (part == QLatin1String(".git")) {
                *error = QStringLiteral("the .git folder is off limits");
                return false;
            }
    }
    return true;
}

QString AgentTools::relative(const QString &absolute) const
{
    const QString r = QDir(m_root).relativeFilePath(absolute);
    return r.isEmpty() ? QStringLiteral(".") : r;
}

bool AgentTools::readText(const QString &absolute, QString *text, QString *error) const
{
    if (!m_hooks.readFile || !QFileInfo::exists(absolute)) {
        *error = QStringLiteral("'%1' does not exist").arg(relative(absolute));
        return false;
    }
    if (!m_hooks.readFile(absolute, text)) {
        *error = QStringLiteral("'%1' cannot be read (binary, larger than 1 MB, or not a file)").arg(relative(absolute));
        return false;
    }
    text->remove(QLatin1Char('\r'));
    return true;
}

QStringList AgentTools::indexedFiles() const
{
    QStringList files = m_hooks.projectFiles ? m_hooks.projectFiles() : QStringList();
    if (files.isEmpty()) { // no index yet: walk the folder
        QDirIterator it(m_root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext() && files.size() < 30000) {
            const QString p = it.next();
            const QStringList parts = QDir(m_root).relativeFilePath(p).split(QLatin1Char('/'));
            bool skip = false;
            for (int i = 0; i + 1 < parts.size(); ++i)
                skip = skip || ignoredDir(parts.at(i));
            if (!skip)
                files << p;
        }
    }
    QStringList out;
    out.reserve(files.size());
    for (const QString &f : std::as_const(files)) {
        if (binarySuffix(f) || AiEnv::isSensitive(f))
            continue;
        bool gone = false;
        for (const QString &g : std::as_const(m_gone))
            gone = gone || f == g || f.startsWith(g + QLatin1Char('/'));
        if (!gone || QFileInfo::exists(f)) // a deletion that was undone brings the file back
            out << f;
    }
    for (const QString &c : m_created)
        if (!out.contains(c) && QFileInfo(c).isFile())
            out << c;
    return out;
}

// Edits live in editor tabs; a command (or git) works on the disk, so save what the agent changed first.
void AgentTools::flushEdits()
{
    if (!m_touched.isEmpty() && m_hooks.saveFiles)
        m_hooks.saveFiles(m_touched.values());
}

bool AgentTools::snapshot(const QString &abs, Before *b) const
{
    b->existed = QFileInfo::exists(abs);
    b->text.clear();
    if (!b->existed)
        return true;
    return m_hooks.readFile && m_hooks.readFile(abs, &b->text);
}

void AgentTools::record(const QString &abs, const Before &b)
{
    if (!m_journal)
        return;
    QString after;
    const bool has = QFileInfo::exists(abs) && m_hooks.readFile && m_hooks.readFile(abs, &after);
    m_journal->recordText(m_turn, relative(abs), b.existed, b.text, has, after);
}

// --- reading --------------------------------------------------------------------------------------------------------

ToolResult AgentTools::listDirectory(const QJsonObject &a)
{
    QString dir, err;
    if (!resolve(str(a, "path"), &dir, &err))
        return fail(err);
    if (!QFileInfo(dir).isDir())
        return fail(QStringLiteral("'%1' is not a folder").arg(relative(dir)));
    QStringList rows;
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &e : entries) {
        if (e.fileName() == QLatin1String(".git"))
            continue;
        rows << (e.isDir() ? e.fileName() + QLatin1Char('/') : QStringLiteral("%1  (%2 bytes)").arg(e.fileName()).arg(e.size()));
        if (rows.size() >= 300)
            break;
    }
    return success({{"path", relative(dir)}, {"entries", rows.join(QLatin1Char('\n'))}, {"count", int(rows.size())}},
                   plural(rows.size(), "entry", "entries"));
}

namespace {
void treeRows(const QString &dir, const QString &prefix, int depth, QStringList *rows)
{
    if (rows->size() >= 400)
        return;
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &e : entries) {
        if (rows->size() >= 400) {
            rows->append(prefix + QStringLiteral("..."));
            return;
        }
        if (e.isDir()) {
            if (ignoredDir(e.fileName())) {
                rows->append(prefix + e.fileName() + QStringLiteral("/  (skipped)"));
                continue;
            }
            rows->append(prefix + e.fileName() + QLatin1Char('/'));
            if (depth > 1)
                treeRows(e.absoluteFilePath(), prefix + QStringLiteral("  "), depth - 1, rows);
        } else {
            rows->append(prefix + e.fileName());
        }
    }
}
}

ToolResult AgentTools::directoryTree(const QJsonObject &a)
{
    QString dir, err;
    if (!resolve(str(a, "path"), &dir, &err))
        return fail(err);
    if (!QFileInfo(dir).isDir())
        return fail(QStringLiteral("'%1' is not a folder").arg(relative(dir)));
    const int depth = qBound(1, a.contains("depth") ? a.value("depth").toInt() : 3, 8);
    QStringList rows;
    treeRows(dir, QString(), depth, &rows);
    return success({{"path", relative(dir)}, {"tree", rows.join(QLatin1Char('\n'))}}, plural(rows.size(), "entry", "entries"));
}

ToolResult AgentTools::findFiles(const QJsonObject &a)
{
    const QString query = str(a, "query").trimmed();
    if (query.isEmpty())
        return fail(QStringLiteral("missing 'query'"));
    QString dir, err;
    if (!resolve(str(a, "path"), &dir, &err))
        return fail(err);
    const QString prefix = dir == m_root ? QString() : relative(dir) + QLatin1Char('/');

    const bool wildcard = query.contains(QLatin1Char('*')) || query.contains(QLatin1Char('?'));
    QRegularExpression re;
    if (wildcard)
        re = QRegularExpression(QRegularExpression::wildcardToRegularExpression(query, QRegularExpression::UnanchoredWildcardConversion),
                                QRegularExpression::CaseInsensitiveOption);
    QVector<QPair<int, QString>> hits;
    for (const QString &f : indexedFiles()) {
        const QString rel = relative(f);
        if (rel.startsWith(QLatin1String("..")) || !rel.startsWith(prefix))
            continue;
        const int nameStart = rel.lastIndexOf(QLatin1Char('/')) + 1;
        if (wildcard) {
            const QString subject = query.contains(QLatin1Char('/')) ? rel : rel.mid(nameStart);
            if (re.match(subject).hasMatch())
                hits.append({-rel.size(), rel});
        } else {
            const int s = FuzzyMatcher::score(query, rel, nameStart);
            if (s >= 0)
                hits.append({s, rel});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const auto &x, const auto &y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
    QStringList out;
    for (int i = 0; i < qMin(40, int(hits.size())); ++i)
        out << hits.at(i).second;
    QJsonObject r{{"files", out.join(QLatin1Char('\n'))}, {"count", int(out.size())}};
    if (hits.size() > 40)
        r.insert("note", QStringLiteral("%1 more matches; use a longer query or a folder").arg(hits.size() - 40));
    return success(r, plural(out.size(), "file", "files"));
}

ToolResult AgentTools::searchText(const QJsonObject &a)
{
    const QString pattern = str(a, "pattern");
    if (pattern.isEmpty())
        return fail(QStringLiteral("missing 'pattern'"));
    QString dir, err;
    if (!resolve(str(a, "path"), &dir, &err))
        return fail(err);
    const QString prefix = dir == m_root ? QString() : relative(dir) + QLatin1Char('/');
    const bool single = QFileInfo(dir).isFile();

    QRegularExpression::PatternOptions opts = a.value("case_sensitive").toBool() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption;
    const QRegularExpression re(a.value("regex").toBool() ? pattern : QRegularExpression::escape(pattern), opts);
    if (!re.isValid())
        return fail(QStringLiteral("invalid regular expression: %1").arg(re.errorString()));

    QRegularExpression glob;
    const QString globText = str(a, "glob").trimmed();
    if (!globText.isEmpty())
        glob = QRegularExpression(QRegularExpression::wildcardToRegularExpression(globText, QRegularExpression::UnanchoredWildcardConversion),
                                  QRegularExpression::CaseInsensitiveOption);
    const int context = qBound(0, a.value("context").toInt(0), 4);
    const int maxResults = qBound(1, a.contains("max_results") ? a.value("max_results").toInt() : 60, 200);

    QStringList files = single ? QStringList{dir} : indexedFiles();
    QStringList rows;
    int found = 0, filesWithHits = 0;
    bool truncated = false;
    QElapsedTimer clock;
    clock.start();
    for (const QString &f : std::as_const(files)) {
        const QString rel = relative(f);
        if (!single && (rel.startsWith(QLatin1String("..")) || !rel.startsWith(prefix)))
            continue;
        if (!globText.isEmpty() && !glob.match(globText.contains(QLatin1Char('/')) ? rel : QFileInfo(rel).fileName()).hasMatch())
            continue;
        if (clock.elapsed() > 6000) {
            truncated = true;
            break;
        }
        QString text, e;
        if (!readText(f, &text, &e))
            continue;
        if (!re.match(text).hasMatch())
            continue;
        const Lines L(text);
        int lastShown = 0;
        bool any = false;
        for (int n = 1; n <= L.count; ++n) {
            if (!re.match(L.line(n)).hasMatch())
                continue;
            if (found >= maxResults) {
                truncated = true;
                break;
            }
            ++found;
            any = true;
            for (int c = qMax(lastShown + 1, n - context); c < n; ++c)
                rows << QStringLiteral("%1-%2- %3").arg(rel).arg(c).arg(shorten(L.line(c), 220));
            rows << QStringLiteral("%1:%2: %3").arg(rel).arg(n).arg(shorten(L.line(n), 220));
            lastShown = n;
            for (int c = n + 1; c <= qMin(L.count, n + context); ++c) {
                if (re.match(L.line(c)).hasMatch())
                    break; // it becomes a match row itself
                rows << QStringLiteral("%1-%2- %3").arg(rel).arg(c).arg(shorten(L.line(c), 220));
                lastShown = c;
            }
        }
        if (any)
            ++filesWithHits;
        if (truncated)
            break;
    }
    QJsonObject r{{"matches", rows.join(QLatin1Char('\n'))}, {"count", found}, {"files", filesWithHits}};
    if (truncated)
        r.insert("note", QStringLiteral("stopped early (limit or time); narrow the pattern, folder or glob"));
    return success(r, found == 0 ? QStringLiteral("no matches") : QStringLiteral("%1 in %2").arg(plural(found, "match", "matches"), plural(filesWithHits, "file", "files")));
}

ToolResult AgentTools::readFileTool(const QJsonObject &a)
{
    QString abs, err, text;
    if (!resolve(str(a, "path"), &abs, &err) || !readText(abs, &text, &err))
        return fail(err);
    const Lines L(text);
    const int start = qMax(1, a.value("start_line").toInt(1));
    const int end = a.contains("end_line") ? a.value("end_line").toInt() : start + kDefaultReadLines - 1;
    if (end < start)
        return fail(QStringLiteral("'end_line' must be >= 'start_line'"));
    if (L.count == 0)
        return success({{"path", relative(abs)}, {"total_lines", 0}, {"content", ""}}, QStringLiteral("empty file"));
    if (start > L.count)
        return fail(QStringLiteral("the file has only %1 lines").arg(L.count));
    int stopped = 0;
    const QString body = numberedRange(L, start, end, &stopped);
    const int last = stopped - 1;
    QJsonObject r{{"path", relative(abs)}, {"total_lines", L.count}, {"start_line", start}, {"end_line", last}, {"content", body}};
    if (last < qMin(end, L.count))
        r.insert("note", QStringLiteral("output limit reached; continue with start_line=%1").arg(stopped));
    else if (last < L.count)
        r.insert("note", QStringLiteral("the file continues up to line %1").arg(L.count));
    return success(r, QStringLiteral("lines %1-%2 of %3").arg(start).arg(last).arg(L.count));
}

ToolResult AgentTools::findTextRange(const QJsonObject &a)
{
    QString abs, err, text;
    if (!resolve(str(a, "path"), &abs, &err) || !readText(abs, &text, &err))
        return fail(err);
    QString needle = str(a, "text");
    needle.remove(QLatin1Char('\r'));
    if (needle.isEmpty())
        return fail(QStringLiteral("missing 'text'"));
    const Lines L(text);
    struct Hit {
        int s, e;
    };
    QVector<Hit> hits;
    if (a.value("regex").toBool()) {
        const QRegularExpression re(needle, QRegularExpression::MultilineOption | (a.value("case_sensitive").toBool() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption));
        if (!re.isValid())
            return fail(QStringLiteral("invalid regular expression: %1").arg(re.errorString()));
        auto it = re.globalMatch(text);
        while (it.hasNext() && hits.size() < 200) {
            const auto m = it.next();
            if (m.capturedLength() > 0)
                hits.append({int(m.capturedStart()), int(m.capturedEnd())});
        }
    } else {
        const Qt::CaseSensitivity cs = a.value("case_sensitive").toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive;
        for (int at = 0; (at = int(text.indexOf(needle, at, cs))) >= 0 && hits.size() < 200; at += needle.size())
            hits.append({at, at + int(needle.size())});
    }
    if (hits.isEmpty())
        return fail(QStringLiteral("no match in %1").arg(relative(abs)));
    QStringList rows;
    for (int i = 0; i < qMin(20, int(hits.size())); ++i) {
        const int first = L.lineOf(hits.at(i).s), last = L.lineOf(qMax(hits.at(i).s, hits.at(i).e - 1));
        rows << QStringLiteral("lines %1-%2: %3").arg(first).arg(last).arg(shorten(L.line(first), 160));
    }
    QJsonObject r{{"path", relative(abs)}, {"matches", rows.join(QLatin1Char('\n'))}, {"count", int(hits.size())}, {"total_lines", L.count}};
    if (hits.size() > 20)
        r.insert("note", QStringLiteral("showing the first 20 matches"));
    return success(r, plural(hits.size(), "match", "matches"));
}

ToolResult AgentTools::openFilesTool(const QJsonObject &)
{
    QStringList rows;
    if (m_hooks.openFiles)
        for (const auto &f : m_hooks.openFiles())
            rows << relative(f.first) + (f.second ? QStringLiteral("  (unsaved changes)") : QString());
    return success({{"open_files", rows.isEmpty() ? QStringLiteral("(none)") : rows.join(QLatin1Char('\n'))}}, plural(rows.size(), "tab", "tabs"));
}

ToolResult AgentTools::gitTool(const QString &what, const QJsonObject &a)
{
    flushEdits();
    QStringList args;
    bool changes = false; // touches the working tree or the index
    auto paths = [&](QStringList *out) -> QString {
        for (const QJsonValue &v : a.value("paths").toArray()) {
            QString abs, err;
            if (!resolve(v.toString(), &abs, &err))
                return err;
            out->append(relative(abs));
        }
        return out->isEmpty() ? QStringLiteral("paths is empty; give files or folders (\".\" = everything)") : QString();
    };
    static const QRegularExpression branchName(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_./-]*$"));

    if (what == QLatin1String("status")) {
        args = {"status", "--short", "--branch"};
    } else if (what == QLatin1String("diff")) {
        args = {"diff", "--no-color"};
        if (a.value("staged").toBool())
            args << "--cached";
        const QString path = str(a, "path").trimmed();
        if (path.isEmpty()) {
            args << "--stat";
        } else {
            QString abs, err;
            if (!resolve(path, &abs, &err))
                return fail(err);
            args << "--" << relative(abs);
        }
    } else if (what == QLatin1String("log")) {
        args = {"log", "--no-color", "--format=%h %an, %ar: %s", "-n", QString::number(qBound(1, a.value("count").toInt(10), 50))};
        const QString path = str(a, "path").trimmed();
        if (!path.isEmpty()) {
            QString abs, err;
            if (!resolve(path, &abs, &err))
                return fail(err);
            args << "--" << relative(abs);
        }
    } else if (what == QLatin1String("add") || what == QLatin1String("unstage")) {
        QStringList list;
        const QString err = paths(&list);
        if (!err.isEmpty())
            return fail(err);
        args = what == QLatin1String("add") ? QStringList{"add", "--"} : QStringList{"restore", "--staged", "--"};
        args += list;
        changes = true;
    } else if (what == QLatin1String("commit")) {
        const QString message = str(a, "message").trimmed();
        if (message.isEmpty())
            return fail(QStringLiteral("a commit message is required"));
        args = {"commit", "-m", message};
        if (a.value("all").toBool())
            args << "-a";
        changes = true;
    } else if (what == QLatin1String("switch")) {
        const QString branch = str(a, "branch").trimmed();
        if (!branchName.match(branch).hasMatch())
            return fail(QStringLiteral("invalid branch name"));
        args = {"switch"};
        if (a.value("create").toBool())
            args << "-c";
        args << branch;
        changes = true;
    } else if (what == QLatin1String("stash")) {
        const QString action = str(a, "action").trimmed();
        if (action == QLatin1String("list")) {
            args = {"stash", "list"};
        } else if (action == QLatin1String("pop")) {
            args = {"stash", "pop"};
            changes = true;
        } else if (action == QLatin1String("push")) {
            args = {"stash", "push"};
            if (!str(a, "message").trimmed().isEmpty())
                args << "-m" << str(a, "message").trimmed();
            changes = true;
        } else {
            return fail(QStringLiteral("action must be push, pop or list"));
        }
    } else {
        return fail(QStringLiteral("unknown git action"));
    }

    QProcess p;
    p.setWorkingDirectory(m_root);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForFinished(30000)) {
        p.kill();
        return fail(QStringLiteral("git did not finish"));
    }
    QString out = QString::fromUtf8(p.readAllStandardOutput());
    const QString errText = QString::fromUtf8(p.readAllStandardError()).trimmed();
    if (changes && m_hooks.changed)
        m_hooks.changed();
    if (p.exitCode() != 0)
        return fail(errText.isEmpty() ? out.trimmed() : errText);
    if (out.size() > 12000)
        out = out.left(12000) + QStringLiteral("\n...(cut; pass a path to narrow it)");
    if (out.trimmed().isEmpty())
        out = what == QLatin1String("status") ? QStringLiteral("(clean)") : what == QLatin1String("diff") ? QStringLiteral("(no changes)") : QStringLiteral("done");
    return success({{"output", out.trimmed()}}, what);
}

// --- editing --------------------------------------------------------------------------------------------------------

ToolResult AgentTools::applyOne(const AiEdits::Edit &edit, const QString &)
{
    if (!m_hooks.apply)
        return fail(QStringLiteral("editing is not available"));
    QString target;
    AiEdits::resolveInside(m_root, edit.path, &target);
    Before before;
    if (!snapshot(target, &before))
        return fail(QStringLiteral("'%1' is too large or not text; the agent does not edit it (the change could not be undone)").arg(edit.path));
    const QVector<AiEdits::Outcome> out = m_hooks.apply({edit});
    if (out.isEmpty() || !out.first().ok)
        return fail((out.isEmpty() ? QStringLiteral("the edit failed") : out.first().note)
                    + QStringLiteral(" (re-read the file: its line numbers may have changed)"));
    const AiEdits::Outcome &o = out.first();
    QString abs;
    AiEdits::resolveInside(m_root, edit.path, &abs);
    m_touched.insert(abs);
    record(abs, before);

    ToolResult r;
    r.change.path = edit.path;
    r.change.op = AiEdits::opName(edit.op);
    r.change.startLine = edit.startLine;
    r.change.endLine = edit.endLine;
    r.change.ok = true;
    r.change.note = o.note;
    r.change.before = o.before.left(60000);
    r.change.after = edit.code.left(60000);
    r.hasChange = true;

    QJsonObject resp{{"ok", true}, {"path", edit.path}, {"result", o.note}};
    QString text, err;
    if (edit.op != AiEdits::Edit::Create && readText(abs, &text, &err)) {
        const Lines L(text);
        const int added = edit.code.isEmpty() ? 0 : edit.code.count(QLatin1Char('\n')) + (edit.code.endsWith(QLatin1Char('\n')) ? 0 : 1);
        const int from = qMax(1, edit.startLine - 2), to = qMin(L.count, edit.startLine + qMax(added, 1) + 1);
        resp.insert("total_lines", L.count);
        if (to >= from)
            resp.insert("now_reads", numberedRange(L, from, qMin(to, from + 39), nullptr, 4000));
    }
    r.response = resp;
    r.summary = o.note;
    return r;
}

ToolResult AgentTools::editRange(const QJsonObject &a)
{
    QString abs, err;
    if (!resolve(str(a, "path"), &abs, &err, true))
        return fail(err);
    if (!a.contains("start_line") || !a.contains("end_line"))
        return fail(QStringLiteral("'start_line' and 'end_line' are required"));
    AiEdits::Edit e;
    e.path = relative(abs);
    e.op = AiEdits::Edit::Replace;
    e.startLine = a.value("start_line").toInt();
    e.endLine = a.value("end_line").toInt();
    e.code = AiEdits::stripLineNumbers(str(a, "new_text"));
    return applyOne(e, QString());
}

ToolResult AgentTools::insertLines(const QJsonObject &a)
{
    QString abs, err;
    if (!resolve(str(a, "path"), &abs, &err, true))
        return fail(err);
    if (!a.contains("before_line"))
        return fail(QStringLiteral("'before_line' is required"));
    if (str(a, "text").isEmpty())
        return fail(QStringLiteral("'text' is empty"));
    AiEdits::Edit e;
    e.path = relative(abs);
    e.op = AiEdits::Edit::Insert;
    e.startLine = e.endLine = a.value("before_line").toInt();
    e.code = AiEdits::stripLineNumbers(str(a, "text"));
    return applyOne(e, QString());
}

ToolResult AgentTools::replaceText(const QJsonObject &a)
{
    QString abs, err, text;
    if (!resolve(str(a, "path"), &abs, &err, true) || !readText(abs, &text, &err))
        return fail(err);
    QString oldText = str(a, "old_text"), newText = str(a, "new_text");
    oldText.remove(QLatin1Char('\r'));
    newText.remove(QLatin1Char('\r'));
    if (oldText.isEmpty())
        return fail(QStringLiteral("'old_text' must not be empty"));
    if (oldText == newText)
        return fail(QStringLiteral("'old_text' and 'new_text' are identical"));
    const Lines L(text);

    QVector<int> at;
    for (int p = 0; (p = int(text.indexOf(oldText, p))) >= 0; p += oldText.size())
        at.append(p);
    if (at.isEmpty()) {
        QString hint = QStringLiteral("old_text was not found; it must match exactly, including indentation");
        const QString firstLine = oldText.section(QLatin1Char('\n'), 0, 0).trimmed();
        if (!firstLine.isEmpty()) {
            const int p = int(text.indexOf(firstLine));
            if (p >= 0)
                hint += QStringLiteral(". Its first line does occur at line %1: read around it and copy the text exactly").arg(L.lineOf(p));
        }
        return fail(hint);
    }
    const bool all = a.value("replace_all").toBool();
    if (at.size() > 1 && !all) {
        QStringList lines;
        for (int i = 0; i < qMin(8, int(at.size())); ++i)
            lines << QString::number(L.lineOf(at.at(i)));
        return fail(QStringLiteral("old_text matches %1 places (lines %2); add surrounding lines to make it unique or set replace_all").arg(at.size()).arg(lines.join(QStringLiteral(", "))));
    }

    // One edit per run of touching lines: the replaced text plus the rest of its first and last line.
    struct Group {
        int startLine, endLine;
        QVector<int> matches; // offsets in the old text
        bool swallowed;       // the match includes a line break, so an empty result really removes lines
    };
    const int len = int(oldText.size());
    QVector<Group> groups;
    for (int s : at) {
        const int e = s + len;
        const int first = L.lineOf(s);
        int last = L.lineOf(qMax(s, e - 1));
        const bool swallowed = e > L.lineEnd(last);
        if (swallowed && e < text.size())
            last = L.lineOf(e);
        if (!groups.isEmpty() && first <= groups.last().endLine) {
            Group &g = groups.last();
            g.endLine = qMax(g.endLine, last);
            g.matches.append(s);
            g.swallowed = g.swallowed || swallowed;
        } else {
            groups.append({first, last, {s}, swallowed});
        }
    }
    QVector<AiEdits::Edit> edits;
    QString beforeAll, afterAll;
    for (const Group &g : std::as_const(groups)) {
        const int from = L.lineStart(g.startLine);
        const int to = qMax(L.lineEnd(g.endLine), g.matches.last() + len);
        QString code;
        int pos = from;
        for (int m : g.matches) {
            code += text.mid(pos, m - pos) + newText;
            pos = m + len;
        }
        code += text.mid(pos, qMax(0, to - pos));
        AiEdits::Edit ed;
        ed.path = relative(abs);
        ed.op = AiEdits::Edit::Replace;
        ed.startLine = g.startLine;
        ed.endLine = g.endLine;
        ed.code = (code.isEmpty() && !g.swallowed) ? QStringLiteral("\n") : code; // "\n" keeps an emptied line, "" removes it
        edits.append(ed);
        const QString sep = QStringLiteral("\n\u22ef\n");
        beforeAll += (beforeAll.isEmpty() ? QString() : sep) + text.mid(from, qMax(0, to - from));
        afterAll += (afterAll.isEmpty() ? QString() : sep) + code;
    }
    if (!m_hooks.apply)
        return fail(QStringLiteral("editing is not available"));
    Before before;
    if (!snapshot(abs, &before))
        return fail(QStringLiteral("'%1' is too large or not text; the agent does not edit it").arg(relative(abs)));
    const QVector<AiEdits::Outcome> out = m_hooks.apply(edits);
    for (const AiEdits::Outcome &o : out)
        if (!o.ok)
            return fail(o.note + QStringLiteral(" (re-read the file and try again)"));
    m_touched.insert(abs);
    record(abs, before);

    ToolResult r;
    r.hasChange = true;
    r.change.path = relative(abs);
    r.change.op = QStringLiteral("replace");
    r.change.startLine = groups.first().startLine;
    r.change.endLine = groups.last().endLine;
    r.change.ok = true;
    r.change.before = beforeAll.left(60000);
    r.change.after = afterAll.left(60000);
    r.change.note = at.size() == 1 ? out.first().note : QStringLiteral("replaced %1 occurrences").arg(at.size());
    QJsonObject resp{{"ok", true}, {"path", relative(abs)}, {"replacements", int(at.size())}};
    QString now, e2;
    if (readText(abs, &now, &e2)) {
        const Lines N(now);
        resp.insert("total_lines", N.count);
        if (groups.size() == 1)
            resp.insert("now_reads", numberedRange(N, qMax(1, groups.first().startLine - 1), qMin(N.count, groups.first().startLine + 25), nullptr, 3000));
    }
    r.response = resp;
    r.summary = r.change.note;
    return r;
}

ToolResult AgentTools::createFile(const QJsonObject &a)
{
    QString abs, err;
    if (!resolve(str(a, "path"), &abs, &err, true))
        return fail(err);
    const QString content = AiEdits::stripLineNumbers(str(a, "content"));
    const bool exists = QFileInfo(abs).exists();
    if (QFileInfo(abs).isDir())
        return fail(QStringLiteral("'%1' is a folder").arg(relative(abs)));
    if (exists && !a.value("overwrite").toBool())
        return fail(QStringLiteral("'%1' already exists; set overwrite=true to replace its content, or edit it").arg(relative(abs)));
    if (!exists) {
        AiEdits::Edit e;
        e.path = relative(abs);
        e.op = AiEdits::Edit::Create;
        e.code = content;
        ToolResult r = applyOne(e, QString());
        if (r.ok)
            m_created.insert(abs);
        return r;
    }
    QString old, e2;
    if (!readText(abs, &old, &e2))
        return fail(e2);
    const Lines L(old);
    AiEdits::Edit e;
    e.path = relative(abs);
    e.code = content;
    if (L.count == 0) {
        e.op = AiEdits::Edit::Insert;
        e.startLine = e.endLine = 1;
    } else {
        e.op = AiEdits::Edit::Replace;
        e.startLine = 1;
        e.endLine = L.count;
    }
    return applyOne(e, QString());
}

ToolResult AgentTools::createDirectory(const QJsonObject &a)
{
    QString abs, err;
    if (!resolve(str(a, "path"), &abs, &err, true))
        return fail(err);
    if (QFileInfo(abs).isFile())
        return fail(QStringLiteral("'%1' is a file").arg(relative(abs)));
    if (!QDir().mkpath(abs))
        return fail(QStringLiteral("could not create '%1'").arg(relative(abs)));
    return success({{"ok", true}, {"path", relative(abs)}}, relative(abs));
}

ToolResult AgentTools::movePath(const QJsonObject &a, bool copy)
{
    QString from, to, err;
    if (!resolve(str(a, "from"), &from, &err, true) || !resolve(str(a, "to"), &to, &err, true))
        return fail(err);
    if (!QFileInfo::exists(from))
        return fail(QStringLiteral("'%1' does not exist").arg(relative(from)));
    if (QFileInfo::exists(to))
        return fail(QStringLiteral("'%1' already exists; choose another target").arg(relative(to)));
    if (to.startsWith(from + QLatin1Char('/')))
        return fail(QStringLiteral("cannot put a folder inside itself"));
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (!copy) {
        flushEdits(); // the buffers follow the file, but the disk copy must hold the agent's edits
        if (!QDir().rename(from, to))
            return fail(QStringLiteral("could not move '%1'").arg(relative(from)));
        m_gone.insert(from);
        if (m_hooks.pathMoved)
            m_hooks.pathMoved(from, to);
        if (m_journal)
            m_journal->recordMove(m_turn, relative(from), relative(to));
    } else {
        flushEdits();
        if (!copyTree(from, to))
            return fail(QStringLiteral("could not copy '%1'").arg(relative(from)));
        if (m_journal)
            m_journal->recordCopy(m_turn, relative(from), relative(to));
    }
    if (QFileInfo(to).isDir()) {
        QDirIterator it(to, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            m_created.insert(it.next());
    } else {
        m_created.insert(to);
    }
    m_touched.remove(from);
    m_gone.remove(to);
    return success({{"ok", true}, {"from", relative(from)}, {"to", relative(to)}}, QStringLiteral("%1 -> %2").arg(relative(from), relative(to)));
}

ToolResult AgentTools::deletePath(const QJsonObject &a)
{
    QString abs, err;
    if (!resolve(str(a, "path"), &abs, &err, true))
        return fail(err);
    if (!QFileInfo::exists(abs) && !QFileInfo(abs).isSymLink())
        return fail(QStringLiteral("'%1' does not exist").arg(relative(abs)));
    flushEdits(); // the trash copy must hold the agent's latest edits
    QString inTrash;
    if (!QFile::moveToTrash(abs, &inTrash))
        return fail(QStringLiteral("could not move '%1' to the trash").arg(relative(abs)));
    if (m_journal)
        m_journal->recordTrash(m_turn, relative(abs), inTrash);
    m_gone.insert(abs);
    m_touched.remove(abs);
    if (m_hooks.pathDeleted)
        m_hooks.pathDeleted(abs);
    return success({{"ok", true}, {"path", relative(abs)}, {"note", "moved to the trash (recoverable)"}}, QStringLiteral("moved to trash"));
}

// --- commands -------------------------------------------------------------------------------------------------------

ToolResult AgentTools::runCommand(const QJsonObject &a)
{
    const QString command = str(a, "command");
    if (command.trimmed().isEmpty())
        return fail(QStringLiteral("missing 'command'"));
    if (const QString denied = commandDenied(command); !denied.isEmpty())
        return fail(QStringLiteral("blocked: %1").arg(denied));
    QString cwd = m_root, err;
    if (a.contains("cwd") && !str(a, "cwd").trimmed().isEmpty() && !resolve(str(a, "cwd"), &cwd, &err))
        return fail(err);
    if (!QFileInfo(cwd).isDir())
        return fail(QStringLiteral("cwd is not a folder"));
    const int timeoutSec = qBound(1, a.contains("timeout_seconds") ? a.value("timeout_seconds").toInt() : 60, 900);

    flushEdits();

    QProcess proc;
    m_process = &proc;
    proc.setWorkingDirectory(cwd);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    env.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
    env.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    proc.setProcessEnvironment(env);
    proc.setChildProcessModifier([] { ::setsid(); }); // own process group, so a timeout or Stop kills the whole tree
    QElapsedTimer clock;
    clock.start();
    proc.start(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command});
    if (!proc.waitForStarted(3000)) {
        m_process = nullptr;
        return fail(QStringLiteral("could not start bash"));
    }
    proc.closeWriteChannel(); // programs that wait for input end instead of hanging

    bool timedOut = false;
    QEventLoop loop;
    QTimer limit, poll;
    limit.setSingleShot(true);
    QObject::connect(&limit, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (m_cancelled)
            loop.quit();
    });
    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
    limit.start(timeoutSec * 1000);
    poll.start(100);
    if (proc.state() != QProcess::NotRunning)
        loop.exec();

    if (proc.state() != QProcess::NotRunning) {
        const pid_t pid = pid_t(proc.processId());
        ::kill(-pid, SIGTERM);
        if (!proc.waitForFinished(2000)) {
            ::kill(-pid, SIGKILL);
            proc.waitForFinished(1000);
        }
    }
    const QByteArray out = proc.readAllStandardOutput(), errOut = proc.readAllStandardError();
    const int code = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
    m_process = nullptr;

    if (m_cancelled)
        return fail(QStringLiteral("cancelled by the user"));
    QJsonObject r{{"exit_code", code}, {"stdout", clipTail(out)}, {"stderr", clipTail(errOut)}, {"seconds", double(clock.elapsed()) / 1000.0}};
    if (timedOut)
        r.insert("timed_out", QStringLiteral("killed after %1 s").arg(timeoutSec));
    ToolResult res = success(r, timedOut ? QStringLiteral("timed out after %1 s").arg(timeoutSec) : QStringLiteral("exit code %1").arg(code));
    res.ok = code == 0 && !timedOut; // the model still gets the output; the transcript shows a failure
    return res;
}

// --- undo / redo / diagnostics ----------------------------------------------------------------------------------------

namespace {
QString entryText(const Journal::Entry &e)
{
    QString s = QStringLiteral("%1 %2").arg(e.what, e.path);
    if (e.what == QLatin1String("edited") || e.what == QLatin1String("created"))
        s += QStringLiteral(" (+%1 -%2)").arg(e.added).arg(e.removed);
    return s;
}

QJsonObject resultJson(const Journal::Result &r)
{
    QJsonObject o;
    if (!r.done.isEmpty())
        o.insert("done", r.done.join(QLatin1Char('\n')));
    if (!r.conflicts.isEmpty()) {
        o.insert("not_done", r.conflicts.join(QLatin1Char('\n')));
        o.insert("note", "Some files were changed after the agent left them (for example by the user), so they were left alone. Tell the user; do not retry.");
    }
    return o;
}
} // namespace

ToolResult AgentTools::listChanges(const QJsonObject &)
{
    if (!m_journal)
        return fail(QStringLiteral("no change history is available"));
    const auto entries = m_journal->entries(Journal::All);
    QStringList rows;
    for (const auto &e : entries)
        rows << QStringLiteral("turn %1: %2").arg(e.turn).arg(entryText(e));
    QJsonObject r{{"changes", rows.isEmpty() ? QStringLiteral("(none: the agent has not changed anything in this chat, or it was undone)") : rows.join(QLatin1Char('\n'))},
                  {"current_turn", m_turn}};
    if (m_journal->canRedo())
        r.insert("can_redo", QStringLiteral("there are undone changes that redo_changes can bring back"));
    return success(r, plural(entries.size(), "change", "changes"));
}

ToolResult AgentTools::undoChanges(const QJsonObject &a)
{
    if (!m_journal)
        return fail(QStringLiteral("no change history is available"));
    Journal::Scope s;
    const QString scope = str(a, "scope").trimmed().toLower();
    s.turn = scope == QLatin1String("all") ? int(Journal::All) : int(Journal::Last);
    if (!str(a, "path").trimmed().isEmpty()) {
        QString abs, err;
        if (!resolve(str(a, "path"), &abs, &err, true))
            return fail(err);
        s.path = relative(abs);
    }
    s.startLine = a.value("start_line").toInt();
    s.endLine = a.contains("end_line") ? a.value("end_line").toInt() : s.startLine;
    flushEdits();
    const Journal::Result r = m_journal->undo(s);
    if (!r.error.isEmpty())
        return fail(r.error);
    if (r.done.isEmpty())
        return fail(r.conflicts.join(QStringLiteral("; ")) + QStringLiteral(" - tell the user"));
    ToolResult out = success(resultJson(r), QStringLiteral("undid %1").arg(plural(r.done.size(), "change", "changes")));
    out.ok = r.conflicts.isEmpty();
    return out;
}

ToolResult AgentTools::redoChanges(const QJsonObject &a)
{
    if (!m_journal)
        return fail(QStringLiteral("no change history is available"));
    QString rel;
    if (!str(a, "path").trimmed().isEmpty()) {
        QString abs, err;
        if (!resolve(str(a, "path"), &abs, &err, true))
            return fail(err);
        rel = relative(abs);
    }
    const Journal::Result r = m_journal->redo(rel);
    if (!r.error.isEmpty())
        return fail(r.error);
    if (r.done.isEmpty())
        return fail(r.conflicts.join(QStringLiteral("; ")) + QStringLiteral(" - tell the user"));
    ToolResult out = success(resultJson(r), QStringLiteral("redid %1").arg(plural(r.done.size(), "change", "changes")));
    out.ok = r.conflicts.isEmpty();
    return out;
}

ToolResult AgentTools::diagnosticsTool(const QJsonObject &a)
{
    if (!m_hooks.diagnostics)
        return fail(QStringLiteral("language server diagnostics are not available"));
    QString abs;
    if (!str(a, "path").trimmed().isEmpty()) {
        QString err;
        if (!resolve(str(a, "path"), &abs, &err))
            return fail(err);
        if (!QFileInfo(abs).isFile())
            return fail(QStringLiteral("'%1' is not a file").arg(relative(abs)));
    }
    flushEdits();
    QVector<Diag> rows;
    const QString note = m_hooks.diagnostics(abs, &rows);
    const QString level = str(a, "severity").trimmed().toLower();
    const int maxSeverity = level == QLatin1String("error") ? 1 : level == QLatin1String("all") ? 4 : 2;
    QStringList lines;
    int errors = 0, warnings = 0, skipped = 0;
    std::sort(rows.begin(), rows.end(), [](const Diag &x, const Diag &y) {
        return x.severity != y.severity ? x.severity < y.severity : x.path != y.path ? x.path < y.path : x.line < y.line;
    });
    for (const Diag &d : std::as_const(rows)) {
        if (d.severity > maxSeverity)
            continue;
        d.severity == 1 ? ++errors : (d.severity == 2 ? ++warnings : 0);
        if (lines.size() >= 60) {
            ++skipped;
            continue;
        }
        static const char *names[] = {"", "error", "warning", "info", "hint"};
        lines << QStringLiteral("%1:%2:%3 %4%5%6: %7")
                     .arg(relative(d.path)).arg(d.line).arg(d.column).arg(QLatin1String(names[qBound(1, d.severity, 4)]))
                     .arg(d.source.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(d.source))
                     .arg(d.code.isEmpty() ? QString() : QStringLiteral(" %1").arg(d.code))
                     .arg(shorten(d.message, 300));
    }
    QJsonObject r{{"diagnostics", lines.isEmpty() ? QStringLiteral("(none)") : lines.join(QLatin1Char('\n'))}, {"errors", errors}, {"warnings", warnings}};
    if (skipped)
        r.insert("note", QStringLiteral("%1 more not shown; pass a path").arg(skipped));
    if (!note.isEmpty())
        r.insert("language_server", note);
    return success(r, !note.isEmpty() && lines.isEmpty() ? note : QStringLiteral("%1, %2").arg(plural(errors, "error", "errors"), plural(warnings, "warning", "warnings")));
}

// --- the table ------------------------------------------------------------------------------------------------------

void AgentTools::registerAll()
{
    auto path = [](const QJsonObject &a) { return str(a, "path").isEmpty() ? QStringLiteral(".") : str(a, "path"); };

    add({"list_directory", "List the files and folders directly inside a folder of the project.",
         makeSchema({{"path", "string", "Folder, relative to the project root. Default: the root.", false}}), Risk::ReadOnly,
         [this](const QJsonObject &a) { return listDirectory(a); },
         [path](const QJsonObject &a) { return QStringLiteral("List %1").arg(path(a)); }, {}});

    add({"directory_tree", "Indented overview of a folder and its sub-folders (dependency and build folders are skipped). Good first call in an unfamiliar project.",
         makeSchema({{"path", "string", "Folder, relative to the project root. Default: the root.", false}, {"depth", "integer", "Levels to show, 1-8. Default 3.", false}}),
         Risk::ReadOnly, [this](const QJsonObject &a) { return directoryTree(a); },
         [path](const QJsonObject &a) { return QStringLiteral("Tree of %1").arg(path(a)); }, {}});

    add({"find_files", "Find files by name. A plain query is a fuzzy match on the path (\"mainwin\" finds src/window/MainWindow.cpp); a query with * or ? is a wildcard on the file name (or on the whole path when it contains a /).",
         makeSchema({{"query", "string", "File name part or wildcard."}, {"path", "string", "Only below this folder. Default: the whole project.", false}}),
         Risk::ReadOnly, [this](const QJsonObject &a) { return findFiles(a); },
         [](const QJsonObject &a) { return QStringLiteral("Find files “%1”").arg(shorten(str(a, "query"), 50)); }, {}});

    add({"search_text", "Search the text of the project files (like grep; unsaved editor buffers are searched, not the disk copy). Returns file:line: text rows. Use it to locate code before reading files.",
         makeSchema({{"pattern", "string", "Text to find, or a regular expression when regex=true."},
                     {"path", "string", "File or folder to search. Default: the whole project.", false},
                     {"regex", "boolean", "Treat pattern as a regular expression. Default false.", false},
                     {"case_sensitive", "boolean", "Default false.", false},
                     {"glob", "string", "Only files whose name matches, e.g. *.cpp or src/**/*.ts.", false},
                     {"context", "integer", "Lines of context around each match, 0-4. Default 0.", false},
                     {"max_results", "integer", "Default 60, max 200.", false}}),
         Risk::ReadOnly, [this](const QJsonObject &a) { return searchText(a); },
         [](const QJsonObject &a) { return QStringLiteral("Search “%1”").arg(shorten(str(a, "pattern"), 50)); }, {}});

    add({"read_file", "Read a text file with line numbers (\"  12 | code\"; the numbers are not part of the file). Returns up to about 400 lines or 16 KB per call; the answer says how to continue. For big files locate the part first with search_text or find_text_range.",
         makeSchema({{"path", "string", "File, relative to the project root."}, {"start_line", "integer", "First line, 1-based. Default 1.", false},
                     {"end_line", "integer", "Last line, inclusive. Default start_line+399.", false}}),
         Risk::ReadOnly, [this](const QJsonObject &a) { return readFileTool(a); },
         [](const QJsonObject &a) {
             QString s = QStringLiteral("Read %1").arg(str(a, "path"));
             if (a.contains("start_line"))
                 s += QStringLiteral(":%1-%2").arg(a.value("start_line").toInt()).arg(a.contains("end_line") ? QString::number(a.value("end_line").toInt()) : QStringLiteral("…"));
             return s;
         },
         {}});

    add({"find_text_range", "Locate a snippet (or regex) in one file and return the line range of every match, e.g. \"lines 40-52\". Use it to get the exact start_line/end_line for edit_range.",
         makeSchema({{"path", "string", "File, relative to the project root."}, {"text", "string", "The text to find (may span several lines), or a regex."},
                     {"regex", "boolean", "Default false.", false}, {"case_sensitive", "boolean", "Default false.", false}}),
         Risk::ReadOnly, [this](const QJsonObject &a) { return findTextRange(a); },
         [](const QJsonObject &a) { return QStringLiteral("Locate text in %1").arg(str(a, "path")); }, {}});

    add({"get_open_files", "The files currently open in editor tabs, marking those with unsaved changes.", makeSchema({}), Risk::ReadOnly,
         [this](const QJsonObject &a) { return openFilesTool(a); }, [](const QJsonObject &) { return QStringLiteral("List open tabs"); }, {}});

    // Every git tool asks the user first (Allow once / Allow for this chat / Deny). Push, reset and discarding changes are not offered.
    auto addGit = [this](const char *name, const char *desc, const QJsonObject &schema, const char *action, std::function<QString(const QJsonObject &)> label) {
        add({QString::fromLatin1(name), QString::fromLatin1(desc), schema, Risk::Dangerous,
             [this, action](const QJsonObject &a) { return gitTool(QString::fromLatin1(action), a); }, label, label});
    };
    auto joined = [](const QJsonObject &a) {
        QStringList l;
        for (const QJsonValue &v : a.value("paths").toArray())
            l << v.toString();
        return l.join(QLatin1Char(' '));
    };
    addGit("git_status", "Branch and changed files (git status --short).", makeSchema({}), "status",
           [](const QJsonObject &) { return QStringLiteral("git status"); });
    addGit("git_diff", "Uncommitted changes: a summary, or the full diff of one file when path is given.",
           makeSchema({{"path", "string", "File to diff. Omit for a summary.", false}, {"staged", "boolean", "Staged changes instead. Default false.", false}}), "diff",
           [](const QJsonObject &a) { return str(a, "path").isEmpty() ? QStringLiteral("git diff") : QStringLiteral("git diff %1").arg(str(a, "path")); });
    addGit("git_log", "Recent commits, one per line.",
           makeSchema({{"count", "integer", "How many, 1-50. Default 10.", false}, {"path", "string", "Only commits touching this path.", false}}), "log",
           [](const QJsonObject &a) { return QStringLiteral("git log -n %1").arg(a.value("count").toInt(10)); });
    addGit("git_add", "Stage files or folders (\".\" = everything).", makeSchema({{"paths", "array", "Paths relative to the project root."}}), "add",
           [joined](const QJsonObject &a) { return QStringLiteral("git add %1").arg(joined(a)); });
    addGit("git_unstage", "Unstage files or folders (keeps the working copy).", makeSchema({{"paths", "array", "Paths relative to the project root."}}), "unstage",
           [joined](const QJsonObject &a) { return QStringLiteral("git restore --staged %1").arg(joined(a)); });
    addGit("git_commit", "Commit the staged changes. Check git_status/git_diff first and write a short imperative message.",
           makeSchema({{"message", "string", "Commit message."}, {"all", "boolean", "Also commit all modified tracked files (-a). Default false.", false}}), "commit",
           [](const QJsonObject &a) { return QStringLiteral("git commit%1 -m \"%2\"").arg(a.value("all").toBool() ? QStringLiteral(" -a") : QString(), str(a, "message")); });
    addGit("git_switch", "Switch to a branch, or create it first.",
           makeSchema({{"branch", "string", "Branch name."}, {"create", "boolean", "Create it (-c). Default false.", false}}), "switch",
           [](const QJsonObject &a) { return QStringLiteral("git switch%1 %2").arg(a.value("create").toBool() ? QStringLiteral(" -c") : QString(), str(a, "branch")); });
    addGit("git_stash", "Stash or restore uncommitted changes.",
           makeSchema({{"action", "string", "push, pop or list."}, {"message", "string", "Label for push.", false}}), "stash",
           [](const QJsonObject &a) { return QStringLiteral("git stash %1").arg(str(a, "action")); });

    add({"edit_range", "Replace lines start_line..end_line (1-based, inclusive) of a file with new_text. An empty new_text deletes the lines. The edit lands in the editor tab as an unsaved, undoable change. Line numbers below the edit shift: after an edit use the numbers in the reply or re-read.",
         makeSchema({{"path", "string", "File, relative to the project root."}, {"start_line", "integer", "First line to replace."}, {"end_line", "integer", "Last line to replace."},
                     {"new_text", "string", "Replacement code without line-number prefixes."}}),
         Risk::Edit, [this](const QJsonObject &a) { return editRange(a); },
         [](const QJsonObject &a) { return QStringLiteral("Edit %1 lines %2-%3").arg(str(a, "path")).arg(a.value("start_line").toInt()).arg(a.value("end_line").toInt()); }, {}});

    add({"replace_text", "Edit a file by replacing an exact snippet (including indentation). old_text must occur exactly once unless replace_all is true. Safer than edit_range after other edits, because it needs no line numbers.",
         makeSchema({{"path", "string", "File, relative to the project root."}, {"old_text", "string", "Exact text to replace, copied from the file; add neighbouring lines to make it unique."},
                     {"new_text", "string", "Replacement."}, {"replace_all", "boolean", "Replace every occurrence. Default false.", false}}),
         Risk::Edit, [this](const QJsonObject &a) { return replaceText(a); },
         [](const QJsonObject &a) { return QStringLiteral("Edit %1").arg(str(a, "path")); }, {}});

    add({"insert_lines", "Insert text in front of line before_line (1-based). Use the line count + 1 to append at the end.",
         makeSchema({{"path", "string", "File, relative to the project root."}, {"before_line", "integer", "The new text ends up in front of this line."}, {"text", "string", "Code to insert."}}),
         Risk::Edit, [this](const QJsonObject &a) { return insertLines(a); },
         [](const QJsonObject &a) { return QStringLiteral("Insert into %1 at line %2").arg(str(a, "path")).arg(a.value("before_line").toInt()); }, {}});

    add({"create_file", "Create a new file with the given content (folders are created as needed). Fails when the file exists unless overwrite=true, which replaces its whole content.",
         makeSchema({{"path", "string", "New file, relative to the project root."}, {"content", "string", "The whole file."}, {"overwrite", "boolean", "Default false.", false}}),
         Risk::Edit, [this](const QJsonObject &a) { return createFile(a); },
         [](const QJsonObject &a) { return QStringLiteral("Create %1").arg(str(a, "path")); }, {}});

    add({"create_directory", "Create a folder (and its parents).", makeSchema({{"path", "string", "Folder, relative to the project root."}}), Risk::Edit,
         [this](const QJsonObject &a) { return createDirectory(a); }, [](const QJsonObject &a) { return QStringLiteral("Create folder %1").arg(str(a, "path")); }, {}});

    add({"move_path", "Move or rename a file or folder inside the project. Fails when the target exists. Open tabs follow the file. Imports, includes and other references are NOT updated: search for them afterwards and fix them yourself.",
         makeSchema({{"from", "string", "Existing path."}, {"to", "string", "New path (the exact new name, not the folder to move into)."}}), Risk::Edit,
         [this](const QJsonObject &a) { return movePath(a, false); },
         [](const QJsonObject &a) { return QStringLiteral("Move %1 → %2").arg(str(a, "from"), str(a, "to")); }, {}});

    add({"copy_path", "Copy a file or folder inside the project. Fails when the target exists.",
         makeSchema({{"from", "string", "Existing path."}, {"to", "string", "New path."}}), Risk::Edit,
         [this](const QJsonObject &a) { return movePath(a, true); },
         [](const QJsonObject &a) { return QStringLiteral("Copy %1 → %2").arg(str(a, "from"), str(a, "to")); }, {}});

    add({"delete_path", "Delete a file or folder of the project (it goes to the trash, so it can be restored). The user is asked to confirm.",
         makeSchema({{"path", "string", "File or folder to delete."}}), Risk::Dangerous,
         [this](const QJsonObject &a) { return deletePath(a); },
         [](const QJsonObject &a) { return QStringLiteral("Delete %1").arg(str(a, "path")); },
         [this](const QJsonObject &a) {
             QString abs, err;
             return QStringLiteral("Move to the trash:\n\n%1").arg(resolve(str(a, "path"), &abs, &err) ? abs : str(a, "path"));
         }});

    add({"run_command", "Run a shell command (bash -c) in the project and return its exit code, stdout and stderr (last 8 KB each). Use it to build, run tests, run linters or git. Unsaved edits of yours are saved first. The user must approve every command. Commands cannot read input; sudo and destructive system commands are blocked.",
         makeSchema({{"command", "string", "The command line."}, {"cwd", "string", "Working folder, relative to the project root. Default: the root.", false},
                     {"timeout_seconds", "integer", "Kill after this many seconds. Default 60, max 900.", false}}),
         Risk::Dangerous, [this](const QJsonObject &a) { return runCommand(a); },
         [](const QJsonObject &a) { return QStringLiteral("Run `%1`").arg(shorten(str(a, "command"), 90)); },
         [this](const QJsonObject &a) {
             QString cwd = m_root, err;
             if (!str(a, "cwd").trimmed().isEmpty())
                 resolve(str(a, "cwd"), &cwd, &err);
             return QStringLiteral("Run in %1:\n\n%2").arg(cwd, str(a, "command"));
         }});

    add({"list_changes", "List what the agent has changed in this chat (per turn), so you can tell what undo_changes would revert.", makeSchema({}), Risk::ReadOnly,
         [this](const QJsonObject &a) { return listChanges(a); }, [](const QJsonObject &) { return QStringLiteral("List changes"); }, {}});

    add({"undo_changes", "Undo changes the agent made in this chat. ALWAYS use this when the user asks to undo, revert, roll back or take back changes - never reconstruct old code by hand. "
                         "scope \"last\" (default) reverts the most recent turn that changed something; \"all\" reverts every change of this chat. With path it only reverts that file; "
                         "with path and start_line/end_line (lines of the file as it is now) it only reverts the changes touching those lines. Created files are removed, moved files go back, "
                         "deleted files are restored from the trash. Files the user changed afterwards are left alone and reported.",
         makeSchema({{"scope", "string", "\"last\" (default) or \"all\".", false}, {"path", "string", "Only this file, relative to the project root.", false},
                     {"start_line", "integer", "With path: first line of the range to revert.", false}, {"end_line", "integer", "With path: last line of the range.", false}}),
         Risk::Edit, [this](const QJsonObject &a) { return undoChanges(a); },
         [](const QJsonObject &a) {
             QString s = QStringLiteral("Undo %1").arg(str(a, "scope") == QLatin1String("all") ? QStringLiteral("all changes") : QStringLiteral("the last changes"));
             if (!str(a, "path").isEmpty())
                 s = QStringLiteral("Undo changes in %1").arg(str(a, "path")) + (a.contains("start_line") ? QStringLiteral(" lines %1-%2").arg(a.value("start_line").toInt()).arg(a.contains("end_line") ? a.value("end_line").toInt() : a.value("start_line").toInt()) : QString());
             return s;
         },
         {}});

    add({"redo_changes", "Bring back changes that undo_changes reverted (the most recent undo first). Use it when the user asks to redo or re-apply what was undone. With path only that file.",
         makeSchema({{"path", "string", "Only this file, relative to the project root.", false}}), Risk::Edit,
         [this](const QJsonObject &a) { return redoChanges(a); },
         [](const QJsonObject &a) { return str(a, "path").isEmpty() ? QStringLiteral("Redo changes") : QStringLiteral("Redo changes in %1").arg(str(a, "path")); }, {}});

    add({"get_diagnostics", "Errors and warnings the language server reports (compile errors, type errors, lint). Pass a path to check one file, or omit it for all open files. "
                            "It waits briefly for the server to analyse your latest edits. Use it after editing to check your work, before or instead of a full build when a server is running.",
         makeSchema({{"path", "string", "File to check. Omit for every open file.", false}, {"severity", "string", "\"error\", \"warning\" (default: errors and warnings) or \"all\".", false}}),
         Risk::ReadOnly, [this](const QJsonObject &a) { return diagnosticsTool(a); },
         [](const QJsonObject &a) { return str(a, "path").isEmpty() ? QStringLiteral("Check diagnostics") : QStringLiteral("Check diagnostics of %1").arg(str(a, "path")); }, {}});
}

} // namespace agent
