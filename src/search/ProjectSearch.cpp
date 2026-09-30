#include "ProjectSearch.h"

#include "project/ProjectFiles.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>

namespace {

constexpr qint64 kMaxFileBytes = 5 * 1024 * 1024;
constexpr int kMaxMatches = 10000;
constexpr int kPreviewChars = 240;

// `*` matches anything (including '/'), `?` one character, `[...]` a class; everything else is literal.
QString globToRegex(const QString &g)
{
    QString out;
    for (int i = 0; i < g.size(); ++i) {
        const QChar c = g.at(i);
        if (c == QLatin1Char('*')) {
            out += QStringLiteral(".*");
        } else if (c == QLatin1Char('?')) {
            out += QLatin1Char('.');
        } else if (c == QLatin1Char('[')) {
            const int close = g.indexOf(QLatin1Char(']'), i + 1);
            if (close < 0) {
                out += QStringLiteral("\\[");
            } else {
                out += g.mid(i, close - i + 1);
                i = close;
            }
        } else {
            out += QRegularExpression::escape(QString(c));
        }
    }
    return out;
}

bool hasWildcard(const QString &g)
{
    return g.contains(QLatin1Char('*')) || g.contains(QLatin1Char('?')) || g.contains(QLatin1Char('['));
}

struct Glob {
    QString raw;
    QRegularExpression rx;
    bool wildcard = false;
    bool usesPath = false;
};

QList<Glob> parseGlobs(const QString &text)
{
    QList<Glob> out;
    for (QString g : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        g = g.trimmed();
        if (g.startsWith(QStringLiteral("./")))
            g = g.mid(2);
        if (g.endsWith(QLatin1Char('/')))
            g += QLatin1Char('*');
        if (g.isEmpty())
            continue;
        Glob gl;
        gl.raw = g;
        gl.wildcard = hasWildcard(g);
        gl.usesPath = g.contains(QLatin1Char('/'));
        if (gl.wildcard)
            gl.rx = QRegularExpression(QRegularExpression::anchoredPattern(globToRegex(g)), QRegularExpression::CaseInsensitiveOption);
        out.append(gl);
    }
    return out;
}

bool globMatches(const Glob &g, const QString &rel, const QString &name)
{
    if (g.wildcard)
        return g.rx.match(g.usesPath ? rel : name).hasMatch();
    // Plain word: a file name, or a directory anywhere in the path.
    return name == g.raw || rel == g.raw || rel.startsWith(g.raw + QLatin1Char('/')) || rel.contains(QLatin1Char('/') + g.raw + QLatin1Char('/'));
}

bool anyMatches(const QList<Glob> &globs, const QString &rel, const QString &name)
{
    for (const Glob &g : globs)
        if (globMatches(g, rel, name))
            return true;
    return false;
}

// Collects the matches of one line into `out`; the preview keeps the match visible on long lines.
void matchLine(const QRegularExpression &rx, const QString &lineIn, int lineNo, QList<SearchMatch> *out, int limit)
{
    QStringView line(lineIn);
    if (line.endsWith(QLatin1Char('\r')))
        line.chop(1);
    auto it = rx.globalMatch(line);
    while (it.hasNext() && out->size() < limit) {
        const auto m = it.next();
        if (m.capturedLength() == 0)
            continue;
        SearchMatch sm;
        sm.line = lineNo;
        sm.column = m.capturedStart();
        sm.length = m.capturedLength();

        int from = 0;
        while (from < sm.column && line.at(from).isSpace()) // drop indentation
            ++from;
        bool cut = false;
        if (sm.column - from > 50) { // keep the match in view on long lines
            from = sm.column - 40;
            cut = true;
        }
        QString text = line.mid(from, kPreviewChars).toString();
        sm.previewStart = sm.column - from;
        if (cut) {
            text.prepend(QChar(0x2026));
            sm.previewStart += 1;
        }
        sm.preview = text;
        out->append(sm);
    }
}

} // namespace

QRegularExpression SearchOptions::toRegex() const
{
    QString pattern = regex ? query : QRegularExpression::escape(query);
    if (wholeWord)
        pattern = QStringLiteral("\\b(?:") + pattern + QStringLiteral(")\\b");
    QRegularExpression::PatternOptions o = QRegularExpression::UseUnicodePropertiesOption;
    if (!caseSensitive)
        o |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(pattern, o);
}

// --- Static helpers -------------------------------------------------------------------------------

bool ProjectSearch::readTextFile(const QString &path, QString *text, bool *hasBom)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    if (f.size() > kMaxFileBytes)
        return false;
    QByteArray data = f.readAll();
    if (data.left(8192).contains('\0'))
        return false;
    bool bom = false;
    if (data.startsWith("\xEF\xBB\xBF")) {
        bom = true;
        data.remove(0, 3);
    }
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString s = dec(data);
    if (dec.hasError())
        return false; // not UTF-8: leave it alone rather than corrupt it
    *text = s;
    if (hasBom)
        *hasBom = bom;
    return true;
}

QString ProjectSearch::expandReplacement(const QString &r, const QRegularExpressionMatch &m)
{
    QString out;
    out.reserve(r.size());
    for (int i = 0; i < r.size(); ++i) {
        const QChar c = r.at(i);
        if (c != QLatin1Char('$') || i + 1 >= r.size()) {
            out += c;
            continue;
        }
        const QChar n = r.at(i + 1);
        if (n == QLatin1Char('$')) {
            out += QLatin1Char('$');
            ++i;
        } else if (n == QLatin1Char('&')) {
            out += m.captured(0);
            ++i;
        } else if (n.isDigit()) {
            out += m.captured(n.digitValue());
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

int ProjectSearch::replaceInText(QString *text, const QRegularExpression &rx, const QString &replacement)
{
    int count = 0;
    QStringList lines = text->split(QLatin1Char('\n'));
    for (QString &line : lines) {
        QString out;
        int last = 0;
        const bool cr = line.endsWith(QLatin1Char('\r'));
        const QStringView view = cr ? QStringView(line).chopped(1) : QStringView(line);
        bool changed = false;
        auto it = rx.globalMatch(view);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.capturedLength() == 0)
                continue;
            out += view.mid(last, m.capturedStart() - last);
            out += expandReplacement(replacement, m);
            last = m.capturedEnd();
            ++count;
            changed = true;
        }
        if (changed) {
            out += view.mid(last);
            if (cr)
                out += QLatin1Char('\r');
            line = out;
        }
    }
    if (count > 0)
        *text = lines.join(QLatin1Char('\n'));
    return count;
}

int ProjectSearch::replaceInDocument(QTextDocument *doc, const QRegularExpression &rx, const QString &replacement)
{
    struct Edit {
        int pos, len;
        QString text;
    };
    QList<Edit> edits;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QString line = b.text();
        auto it = rx.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.capturedLength() == 0)
                continue;
            edits.append({int(b.position() + m.capturedStart()), int(m.capturedLength()), expandReplacement(replacement, m)});
        }
    }
    if (edits.isEmpty())
        return 0;
    QTextCursor c(doc);
    c.beginEditBlock();
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) { // back to front keeps earlier offsets valid
        c.setPosition(it->pos);
        c.setPosition(it->pos + it->len, QTextCursor::KeepAnchor);
        c.insertText(it->text);
    }
    c.endEditBlock();
    return edits.size();
}

// --- The searcher -------------------------------------------------------------------------------------

ProjectSearch::ProjectSearch(QObject *parent)
    : QObject(parent)
{
}

ProjectSearch::~ProjectSearch()
{
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true);
    for (QThread *t : std::as_const(m_threads)) {
        t->wait();
        delete t;
    }
}

void ProjectSearch::cancel()
{
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true);
    m_running = false;
}

void ProjectSearch::start(const QString &root, const SearchOptions &options, const QHash<QString, QString> &overrides)
{
    cancel();
    const QRegularExpression rx = options.toRegex();
    if (options.query.isEmpty()) {
        emit finished(0, 0, false, QString());
        return;
    }
    if (!rx.isValid()) {
        emit finished(0, 0, false, rx.errorString());
        return;
    }
    if (root.isEmpty()) {
        emit finished(0, 0, false, QString());
        return;
    }

    m_running = true;
    const int gen = m_generation;
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancelFlag;
    QPointer<ProjectSearch> self(this);

    QThread *thread = QThread::create([self, root, options, overrides, rx, gen, cancelFlag] {
        const QList<Glob> include = parseGlobs(options.include), exclude = parseGlobs(options.exclude);
        const QDir rootDir(root);
        QStringList files = ProjectFiles::scan(root);
        // Unsaved buffers that live in the project but are not on disk yet still count.
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
            if (it.key().startsWith(root + QLatin1Char('/')) && !files.contains(it.key()))
                files.append(it.key());

        QList<SearchFileResult> batch;
        QElapsedTimer sinceFlush;
        sinceFlush.start();
        int filesWithMatches = 0, totalMatches = 0;
        bool truncated = false;

        auto flush = [&] {
            if (batch.isEmpty())
                return;
            QMetaObject::invokeMethod(
                self.data(), [self, gen, b = std::move(batch)] { if (self) self->onBatch(gen, b); }, Qt::QueuedConnection);
            batch = {};
            sinceFlush.restart();
        };

        // Fast pre-check for plain queries: skip files that cannot contain the text at all.
        const Qt::CaseSensitivity cs = options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
        const bool prefilter = !options.regex;

        for (const QString &path : std::as_const(files)) {
            if (cancelFlag->load() || truncated)
                break;
            const QString rel = rootDir.relativeFilePath(path);
            const QString name = QFileInfo(path).fileName();
            if (!include.isEmpty() && !anyMatches(include, rel, name))
                continue;
            if (!exclude.isEmpty() && anyMatches(exclude, rel, name))
                continue;

            QString text;
            const auto ov = overrides.constFind(path);
            if (ov != overrides.cend())
                text = ov.value();
            else if (!ProjectSearch::readTextFile(path, &text))
                continue;
            if (prefilter && !text.contains(options.query, cs))
                continue;

            SearchFileResult fr;
            fr.path = path;
            int lineNo = 0, start = 0;
            while (start <= text.size() && fr.matches.size() < 2000) {
                int end = text.indexOf(QLatin1Char('\n'), start);
                if (end < 0)
                    end = text.size();
                ++lineNo;
                if (end > start)
                    matchLine(rx, text.mid(start, end - start), lineNo, &fr.matches, 2000);
                if (totalMatches + fr.matches.size() >= kMaxMatches) {
                    truncated = true;
                    break;
                }
                start = end + 1;
            }
            if (!fr.matches.isEmpty()) {
                totalMatches += fr.matches.size();
                ++filesWithMatches;
                batch.append(std::move(fr));
                if (batch.size() >= 40 || sinceFlush.elapsed() > 80)
                    flush();
            }
        }
        flush();
        const bool wasCancelled = cancelFlag->load();
        if (!wasCancelled)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, filesWithMatches, totalMatches, truncated] { if (self) self->onDone(gen, filesWithMatches, totalMatches, truncated); },
                Qt::QueuedConnection);
    });
    m_threads.append(thread);
    connect(thread, &QThread::finished, this, [this, thread] {
        m_threads.removeAll(thread);
        thread->deleteLater();
    });
    thread->start();
}

void ProjectSearch::onBatch(int generation, const QList<SearchFileResult> &batch)
{
    if (generation == m_generation)
        emit results(batch);
}

void ProjectSearch::onDone(int generation, int files, int matches, bool truncated)
{
    if (generation != m_generation)
        return;
    m_running = false;
    emit finished(files, matches, truncated, QString());
}
