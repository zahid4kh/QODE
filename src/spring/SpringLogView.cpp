#include "spring/SpringLogView.h"

#include "settings/SettingsManager.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

namespace {

constexpr int kMaxEntries = 6000;
constexpr int kTrimEntries = 1000;

int rank(SpringLogView::Level l)
{
    switch (l) {
    case SpringLogView::Trace: return 0;
    case SpringLogView::Debug: return 1;
    case SpringLogView::Info: return 2;
    case SpringLogView::Warn: return 3;
    case SpringLogView::Error: return 4;
    case SpringLogView::Build:
    case SpringLogView::Output: return 2;
    case SpringLogView::Marker: return 99;
    }
    return 2;
}

SpringLogView::Level levelOf(const QString &name)
{
    if (name == QLatin1String("TRACE"))
        return SpringLogView::Trace;
    if (name == QLatin1String("DEBUG"))
        return SpringLogView::Debug;
    if (name == QLatin1String("WARN") || name == QLatin1String("WARNING"))
        return SpringLogView::Warn;
    if (name == QLatin1String("ERROR") || name == QLatin1String("FATAL"))
        return SpringLogView::Error;
    return SpringLogView::Info;
}

QString levelTag(SpringLogView::Level l)
{
    switch (l) {
    case SpringLogView::Trace: return QStringLiteral("TRACE");
    case SpringLogView::Debug: return QStringLiteral("DEBUG");
    case SpringLogView::Info: return QStringLiteral("INFO ");
    case SpringLogView::Warn: return QStringLiteral("WARN ");
    case SpringLogView::Error: return QStringLiteral("ERROR");
    default: return QStringLiteral("     ");
    }
}

// "2026-10-15T10:31:22.123+02:00" -> "10:31:22.123"
QString shortTime(const QString &t)
{
    int i = t.indexOf(QLatin1Char('T'));
    if (i < 0)
        i = t.indexOf(QLatin1Char(' '));
    QString s = i >= 0 ? t.mid(i + 1) : t;
    const int zone = s.indexOf(QRegularExpression(QStringLiteral("[Z+-]")));
    if (zone > 0)
        s.truncate(zone);
    return s.left(12);
}

bool isFrameworkPackage(const QString &fqn)
{
    static const char *prefixes[] = {"java.",   "javax.", "jakarta.", "jdk.",   "sun.",     "com.sun.", "kotlin.",   "kotlinx.",
                                     "org.springframework.", "org.apache.", "io.netty.", "org.junit.", "org.eclipse.", "ch.qos.",
                                     "org.slf4j.", "tools.jackson.", "com.fasterxml.", "reactor.", "org.hibernate.", "com.zaxxer."};
    if (fqn.contains(QLatin1Char('/'))) // "java.base/java.lang.Thread.run": a JDK module
        return true;
    for (const char *p : prefixes)
        if (fqn.startsWith(QLatin1String(p)))
            return true;
    return false;
}

} // namespace

struct SpringLogView::Line
{
    struct Seg
    {
        QString text;
        QTextCharFormat fmt;
    };
    QList<Seg> segs;
    LineInfo info;
    int hang = 0; // characters of hanging indent for wrapped text
};

SpringLogView::SpringLogView(QWidget *parent) : QTextEdit(parent), m_flush(new QTimer(this))
{
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setCursorWidth(0);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    QFont f = SettingsManager::instance().editorFont();
    f.setPointSize(qMax(8, f.pointSize() - 1));
    setFont(f);
    m_flush->setSingleShot(true);
    m_flush->setInterval(350);
    connect(m_flush, &QTimer::timeout, this, &SpringLogView::flushPartial);
    updateTheme();
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        updateTheme();
        renderAll();
    });
}

void SpringLogView::updateTheme()
{
    m_theme = Theme::byName(SettingsManager::instance().theme());
    // QTextEdit (not QPlainTextEdit: wrapped lines hang under the message, which needs block margins) is not covered by the
    // application's theme style sheet.
    setStyleSheet(QStringLiteral("SpringLogView { background: %1; color: %2; border: none; padding: 4px; selection-background-color: %3; }")
                      .arg(m_theme.editorBg.name(), m_theme.editorFg.name(), m_theme.selection.name()));
}

void SpringLogView::setResolver(const Resolver &resolver)
{
    m_resolver = resolver;
    clearResolveCache();
}

void SpringLogView::clearResolveCache()
{
    m_resolved.clear();
}

// --- splitting the output into entries -----------------------------------------------------------------------------

void SpringLogView::feed(const QString &chunk)
{
    m_partial += chunk;
    int nl;
    while ((nl = m_partial.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_partial.left(nl);
        m_partial.remove(0, nl + 1);
        processLine(line);
    }
    if (m_partial.isEmpty())
        m_flush->stop();
    else
        m_flush->start(); // a prompt-like line without a newline is shown after a short pause
    sync();
}

void SpringLogView::flushPartial()
{
    if (m_partial.isEmpty())
        return;
    const QString line = m_partial;
    m_partial.clear();
    processLine(line);
    sync();
}

bool SpringLogView::continues(const QString &line) const
{
    static const QRegularExpression more(QStringLiteral(R"(^\s*\.\.\. \d+ (?:more|common frames omitted))"));
    static const QRegularExpression caused(QStringLiteral(R"(^(?:Caused by|Suppressed|Wrapped by):)"));
    static const QRegularExpression exception(QStringLiteral(R"(^[\w$]+(?:\.[\w$]+)+(?:Exception|Error|Throwable)\b)"));
    if (m_entries.isEmpty())
        return false;
    const Entry &last = m_entries.last();
    if (!last.spring)
        return false;
    if (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t')))
        return true;
    return more.match(line).hasMatch() || caused.match(line).hasMatch() || exception.match(line).hasMatch();
}

void SpringLogView::processLine(QString line)
{
    // Spring Boot's default pattern: 2026-10-15T10:31:22.123+02:00  INFO 4711 --- [demo] [  main] c.e.d.Application : Started ...
    static const QRegularExpression spring(
        QStringLiteral(R"(^(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:[.,]\d+)?(?:Z|[+-]\d{2}:?\d{2})?)\s+(TRACE|DEBUG|INFO|WARN|ERROR|FATAL)\s+(?:\d+\s+)?---\s+((?:\[[^\]]*\]\s*)*)(\S+)\s*:\s?(.*)$)"));
    // Plain Logback: 10:31:22.123 [main] INFO  c.e.d.Application - message
    static const QRegularExpression logback(
        QStringLiteral(R"(^(\d{2}:\d{2}:\d{2}[.,]\d+)\s+\[([^\]]*)\]\s+(TRACE|DEBUG|INFO|WARN|ERROR)\s+(\S+)\s+-\s+(.*)$)"));
    static const QRegularExpression bracket(QStringLiteral(R"(\[([^\]]*)\])"));
    static const QRegularExpression build(QStringLiteral(
        R"(^(?:> (?:Task|Configure|Transform)\b|BUILD |Deprecated Gradle|FAILURE:|\* (?:What went wrong|Try|Exception is|Get more help)|\[(?:INFO|WARNING|ERROR)\]|Starting a Gradle Daemon|Reusing configuration|Downloading |Daemon will be stopped|Picked up |\d+ actionable tasks?|Calculating task graph|Welcome to Gradle|[ew]: |Execution failed for task|Process 'command))"));
    static const QRegularExpression kotlinLoc(QStringLiteral(R"(^[ew]: (?:file://)?(/[^:]+?):(\d+):(\d+))"));
    static const QRegularExpression javaLoc(QStringLiteral(R"(^(/[^:\s]+\.(?:java|kt)):(\d+): (?:error|warning))"));
    static const QRegularExpression bootRun(QStringLiteral(R"(^> Task :(?:[\w-]+:)*bootRun\b)"));
    static const QRegularExpression failedBlock(QStringLiteral(R"(^\*{5,}$|^APPLICATION FAILED TO START$)"));

    line.remove(QLatin1Char('\r'));
    line.replace(QLatin1Char('\t'), QStringLiteral("    "));
    if (line.trimmed().isEmpty())
        return;

    Entry e;
    e.raw << line;
    QRegularExpressionMatch m = spring.match(line);
    if (m.hasMatch()) {
        e.spring = true;
        e.level = levelOf(m.captured(2));
        e.time = shortTime(m.captured(1));
        QString thread;
        for (QRegularExpressionMatchIterator it = bracket.globalMatch(m.captured(3)); it.hasNext();)
            thread = it.next().captured(1).trimmed();
        e.thread = thread;
        e.logger = m.captured(4);
        e.text = m.captured(5);
        m_inApp = true;
    } else if ((m = logback.match(line)).hasMatch()) {
        e.spring = true;
        e.level = levelOf(m.captured(3));
        e.time = shortTime(m.captured(1));
        e.thread = m.captured(2);
        e.logger = m.captured(4);
        e.text = m.captured(5);
        m_inApp = true;
    } else if (continues(line)) {
        Entry &last = m_entries.last();
        last.extra << line;
        last.raw << line;
        last.haystack += QLatin1Char('\n') + line.toLower();
        touchLast();
        return;
    } else {
        e.text = line;
        if (build.match(line).hasMatch() || !m_inApp) {
            e.level = Build;
            if (bootRun.match(line).hasMatch())
                m_inApp = true;
            QRegularExpressionMatch lm = kotlinLoc.match(line);
            if (!lm.hasMatch())
                lm = javaLoc.match(line);
            if (lm.hasMatch()) {
                e.loc.kind = LineInfo::Location;
                e.loc.path = lm.captured(1);
                e.loc.line = lm.captured(2).toInt();
                e.loc.column = lm.captured(3).toInt();
            }
        } else {
            e.level = Output;
            e.bad = failedBlock.match(line).hasMatch();
        }
    }
    e.haystack = (e.logger + QLatin1Char(' ') + e.text).toLower();
    addEntry(e);
}

void SpringLogView::addEntry(Entry e)
{
    m_entries.append(e);
    m_dirtyFrom = qMin(m_dirtyFrom, int(m_entries.size()) - 1);
    if (m_entries.size() > kMaxEntries) {
        m_entries.erase(m_entries.begin(), m_entries.begin() + kTrimEntries);
        m_dirtyFrom = 0;
        m_stale = true;
    }
}

void SpringLogView::touchLast()
{
    m_dirtyFrom = qMin(m_dirtyFrom, int(m_entries.size()) - 1);
}

void SpringLogView::addMarker(const QString &text)
{
    Entry e;
    e.level = Marker;
    e.text = text;
    e.raw << text;
    e.haystack = text.toLower();
    m_inApp = false;
    m_partial.clear();
    addEntry(e);
    sync();
}

void SpringLogView::jumpToEnd()
{
    QScrollBar *sb = verticalScrollBar();
    sb->setValue(sb->maximum());
    // The document lays out lazily: once more after the pending layout.
    QTimer::singleShot(0, this, [this] { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); });
}

void SpringLogView::clearLog()
{
    m_entries.clear();
    m_shown.clear();
    m_dirtyFrom = 0;
    m_partial.clear();
    m_stale = false;
    document()->clear();
    emit countsChanged();
}

// --- filters -------------------------------------------------------------------------------------------------------

bool SpringLogView::passes(const Entry &e) const
{
    if (e.level == Marker)
        return true;
    if (e.level == Build && !m_showBuild)
        return false;
    if (rank(e.level) < rank(m_minLevel))
        return false;
    if (!m_filter.isEmpty() && !e.haystack.contains(m_filter))
        return false;
    return true;
}

void SpringLogView::setMinLevel(Level level)
{
    if (m_minLevel == level)
        return;
    m_minLevel = level;
    renderAll();
}

void SpringLogView::setTextFilter(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == m_filter)
        return;
    m_filter = t;
    renderAll();
}

void SpringLogView::setShowBuild(bool show)
{
    if (m_showBuild == show)
        return;
    m_showBuild = show;
    renderAll();
}

void SpringLogView::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    if (!paused)
        renderAll();
}

QString SpringLogView::rawText(bool onlyShown) const
{
    QStringList out;
    for (const Entry &e : m_entries) {
        if (onlyShown && !passes(e))
            continue;
        out << e.raw.join(QLatin1Char('\n'));
    }
    return out.join(QLatin1Char('\n'));
}

// --- drawing -------------------------------------------------------------------------------------------------------

void SpringLogView::sync()
{
    if (m_paused) {
        emit countsChanged();
        return;
    }
    if (m_stale) {
        renderAll();
        return;
    }
    QScrollBar *sb = verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 3;
    for (int i = m_dirtyFrom; i < m_entries.size(); ++i) {
        if (m_entries.at(i).pos >= 0)
            removeTail(i); // an entry that just got more lines is always the last one drawn
        if (passes(m_entries.at(i)))
            drawAtEnd(i);
    }
    m_dirtyFrom = int(m_entries.size());
    if (atBottom)
        sb->setValue(sb->maximum());
    emit countsChanged();
}

void SpringLogView::renderAll()
{
    if (m_paused) {
        emit countsChanged();
        return;
    }
    document()->clear();
    m_shown.clear();
    for (Entry &e : m_entries) {
        e.pos = -1;
        e.len = 0;
    }
    QTextCursor guard(document());
    guard.beginEditBlock();
    for (int i = 0; i < m_entries.size(); ++i)
        if (passes(m_entries.at(i)))
            drawAtEnd(i);
    guard.endEditBlock();
    m_dirtyFrom = int(m_entries.size());
    m_stale = false;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    emit countsChanged();
}

void SpringLogView::removeTail(int index)
{
    Entry &e = m_entries[index];
    QTextCursor c(document());
    c.beginEditBlock();
    if (e.pos > 0) {
        c.setPosition(e.pos - 1); // the separator in front of the entry goes too
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    } else {
        c.select(QTextCursor::Document);
    }
    c.removeSelectedText();
    c.endEditBlock();
    e.pos = -1;
    e.len = 0;
    if (!m_shown.isEmpty() && m_shown.last() == index)
        m_shown.removeLast();
}

void SpringLogView::drawAtEnd(int index)
{
    Entry &e = m_entries[index];
    const QList<Line> lines = buildLines(e);
    QTextCursor c(document());
    c.beginEditBlock();
    c.movePosition(QTextCursor::End);
    const bool first = document()->isEmpty();
    e.pos = c.position() + (first ? 0 : 1);
    e.len = insertLines(c, lines, first);
    c.endEditBlock();
    m_shown.append(index);
}

int SpringLogView::insertLines(QTextCursor &c, const QList<Line> &lines, bool first)
{
    int len = 0;
    for (int i = 0; i < lines.size(); ++i) {
        if (!(i == 0 && first)) {
            c.insertBlock();
            if (i > 0)
                ++len;
        }
        // Every block gets its format explicitly: a new block would otherwise copy the previous line's hanging indent.
        QTextBlockFormat bf;
        if (lines.at(i).hang > 0) {
            const qreal px = QFontMetricsF(font()).horizontalAdvance(QLatin1Char('0')) * lines.at(i).hang;
            bf.setLeftMargin(px);
            bf.setTextIndent(-px);
        }
        c.setBlockFormat(bf);
        for (const Line::Seg &s : lines.at(i).segs) {
            c.insertText(s.text, s.fmt);
            len += int(s.text.size());
        }
    }
    return len;
}

// Draws one entry again (its fold state changed) and moves everything behind it.
void SpringLogView::rerender(int index)
{
    Entry &e = m_entries[index];
    const int slot = int(m_shown.indexOf(index));
    if (e.pos < 0 || slot < 0)
        return;
    const bool last = slot == m_shown.size() - 1;
    if (e.pos == 0 && !last) {
        renderAll();
        return;
    }
    QScrollBar *sb = verticalScrollBar();
    const int scroll = sb->value();
    const QList<Line> lines = buildLines(e);
    QTextCursor c(document());
    c.beginEditBlock();
    if (e.pos > 0) {
        c.setPosition(e.pos - 1);
        c.setPosition(e.pos + e.len, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(e.pos - 1);
    } else {
        c.select(QTextCursor::Document);
        c.removeSelectedText();
    }
    const int newLen = insertLines(c, lines, e.pos == 0);
    c.endEditBlock();
    const int delta = newLen - e.len;
    e.len = newLen;
    if (delta != 0)
        for (int i = slot + 1; i < m_shown.size(); ++i)
            m_entries[m_shown.at(i)].pos += delta;
    sb->setValue(scroll);
}

QString SpringLogView::resolveFrame(const QString &fqn, const QString &file) const
{
    if (!m_resolver || isFrameworkPackage(fqn))
        return {};
    QStringList parts = fqn.split(QLatin1Char('.'));
    QStringList pkg;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString &p = parts.at(i);
        if (p.isEmpty() || !(p.at(0).isLower() || p.at(0) == QLatin1Char('_')))
            break;
        pkg << p;
    }
    const QString dir = pkg.join(QLatin1Char('/'));
    const QString key = dir + QLatin1Char('|') + file;
    if (const auto it = m_resolved.constFind(key); it != m_resolved.constEnd())
        return it.value();
    const QString path = m_resolver(dir, file);
    m_resolved.insert(key, path);
    return path;
}

QList<SpringLogView::Line> SpringLogView::buildLines(Entry &e) const
{
    static const QRegularExpression frameRe(QStringLiteral(R"(^\s+at\s+(\S+?)\(([^():]*?)(?::(\d+))?\)(?:\s+(~?\[.*\]))?\s*$)"));
    static const QRegularExpression moreRe(QStringLiteral(R"(^\s*\.\.\. \d+ (?:more|common frames omitted))"));

    const Theme &t = m_theme;
    auto fmt = [](const QColor &c, bool bold = false, bool underline = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)
            f.setFontWeight(QFont::Bold);
        if (underline)
            f.setFontUnderline(true);
        return f;
    };
    auto seg = [](Line &l, const QString &text, const QTextCharFormat &f) { l.segs.append({text, f}); };

    QColor levelColor = t.textMuted;
    QColor messageColor = t.editorFg;
    switch (e.level) {
    case Trace:
    case Debug: messageColor = t.textMuted; break;
    case Info: levelColor = t.success; break;
    case Warn: levelColor = messageColor = t.warning; break;
    case Error: levelColor = messageColor = t.danger; break;
    case Build: messageColor = t.comment; break;
    default: break;
    }

    QList<Line> out;
    Line head;
    if (e.level == Marker) {
        seg(head, QStringLiteral("── %1 ──").arg(e.text), fmt(t.textMuted, true));
    } else if (e.spring) {
        // "15:10:39  INFO  message  logger": the message first, because the panel is narrow; wrapped lines hang under it.
        const QString prefix = e.time.left(8) + QStringLiteral("  ");
        seg(head, prefix, fmt(t.textMuted));
        seg(head, levelTag(e.level) + QLatin1Char(' '), fmt(levelColor, true));
        seg(head, e.text, fmt(messageColor));
        seg(head, QStringLiteral("  ") + e.logger, fmt(t.function));
        head.hang = int(prefix.size()) + 6;
    } else if (e.loc.kind == LineInfo::Location) {
        seg(head, e.text, fmt(t.accent, false, true));
        head.info = e.loc;
    } else {
        seg(head, e.text, fmt(e.bad ? t.danger : messageColor, e.bad));
    }
    out.append(head);

    if (!e.extra.isEmpty()) {
        enum Kind { Other, UserFrame, FrameworkFrame, More };
        struct Item { Kind kind; QString text; QString fqn, file, jar; int line = 0; QString path; };
        QList<Item> items;
        int hidden = 0;
        for (const QString &raw : std::as_const(e.extra)) {
            Item it;
            it.text = raw;
            QRegularExpressionMatch fm = frameRe.match(raw);
            if (fm.hasMatch()) {
                it.fqn = fm.captured(1);
                it.file = fm.captured(2);
                it.line = fm.captured(3).toInt();
                it.jar = fm.captured(4);
                if (it.line > 0)
                    it.path = resolveFrame(it.fqn, it.file);
                it.kind = it.path.isEmpty() ? FrameworkFrame : UserFrame;
            } else if (moreRe.match(raw).hasMatch()) {
                it.kind = More;
            } else {
                it.kind = Other;
            }
            if (it.kind == FrameworkFrame || it.kind == More)
                ++hidden;
            items.append(it);
        }
        for (const Item &it : std::as_const(items)) {
            if (!e.expanded && (it.kind == FrameworkFrame || it.kind == More))
                continue;
            Line l;
            switch (it.kind) {
            case UserFrame:
            case FrameworkFrame: {
                const bool user = it.kind == UserFrame;
                seg(l, QStringLiteral("    at "), fmt(t.textMuted));
                seg(l, it.fqn, fmt(user ? t.editorFg : t.textMuted, user));
                seg(l, QStringLiteral("("), fmt(t.textMuted));
                const QString where = it.line > 0 ? QStringLiteral("%1:%2").arg(it.file).arg(it.line) : it.file;
                seg(l, where, user ? fmt(t.accent, false, true) : fmt(t.textMuted));
                seg(l, QStringLiteral(")"), fmt(t.textMuted));
                if (!it.jar.isEmpty())
                    seg(l, QLatin1Char(' ') + it.jar, fmt(t.textMuted));
                l.hang = 8;
                if (user) {
                    l.info.kind = LineInfo::Location;
                    l.info.path = it.path;
                    l.info.line = it.line;
                }
                break;
            }
            case More:
                seg(l, it.text.trimmed().prepend(QStringLiteral("    ")), fmt(t.textMuted));
                break;
            case Other: {
                const bool exc = !it.text.startsWith(QLatin1Char(' '));
                seg(l, it.text, fmt(exc ? t.danger : messageColor));
                break;
            }
            }
            out.append(l);
        }
        if (hidden > 0) {
            Line l;
            l.info.kind = LineInfo::Toggle;
            seg(l,
                e.expanded ? QStringLiteral("    %1 hide framework frames").arg(QChar(0x25BE))
                           : QStringLiteral("    %1 %2 more line%3 — click to show").arg(QChar(0x25B8)).arg(hidden).arg(hidden == 1 ? QString() : QStringLiteral("s")),
                fmt(t.textMuted, false, true));
            out.append(l);
        }
    }

    e.lines.clear();
    for (const Line &l : std::as_const(out))
        e.lines.append(l.info);
    return out;
}

// --- mouse ---------------------------------------------------------------------------------------------------------

SpringLogView::LineInfo SpringLogView::infoAt(const QPoint &viewportPos, int *entry) const
{
    if (m_shown.isEmpty())
        return {};
    const int p = cursorForPosition(viewportPos).position();
    int lo = 0, hi = int(m_shown.size()) - 1, hit = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_entries.at(m_shown.at(mid)).pos <= p) {
            hit = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (hit < 0)
        return {};
    const Entry &e = m_entries.at(m_shown.at(hit));
    if (p > e.pos + e.len)
        return {};
    const int line = document()->findBlock(p).blockNumber() - document()->findBlock(e.pos).blockNumber();
    if (line < 0 || line >= e.lines.size())
        return {};
    if (entry)
        *entry = m_shown.at(hit);
    return e.lines.at(line);
}

void SpringLogView::mouseMoveEvent(QMouseEvent *event)
{
    QTextEdit::mouseMoveEvent(event);
    if (event->buttons() == Qt::NoButton)
        viewport()->setCursor(infoAt(event->pos()).kind != LineInfo::Plain ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void SpringLogView::leaveEvent(QEvent *event)
{
    viewport()->setCursor(Qt::IBeamCursor);
    QTextEdit::leaveEvent(event);
}

void SpringLogView::mousePressEvent(QMouseEvent *event)
{
    m_pressPos = event->pos();
    QTextEdit::mousePressEvent(event);
}

void SpringLogView::mouseReleaseEvent(QMouseEvent *event)
{
    QTextEdit::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton || (event->pos() - m_pressPos).manhattanLength() > 4 || textCursor().hasSelection())
        return;
    int entry = -1;
    const LineInfo info = infoAt(event->pos(), &entry);
    if (info.kind == LineInfo::Location) {
        emit locationRequested(info.path, info.line, info.column);
    } else if (info.kind == LineInfo::Toggle && entry >= 0) {
        m_entries[entry].expanded = !m_entries.at(entry).expanded;
        rerender(entry);
    }
}
