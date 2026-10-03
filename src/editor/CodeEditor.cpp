#include "CodeEditor.h"

#include "Breadcrumbs.h"
#include "CompletionPopup.h"
#include "MiniMap.h"
#include "SyntaxHighlighter.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QPointer>
#include <QApplication>
#include <QHelpEvent>
#include <QToolTip>
#include "HoverPopup.h"
#include <QDateTime>
#include <QPainterPath>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace {

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *e) : QWidget(e), m_editor(e) { setMouseTracking(true); }
    QSize sizeHint() const override { return QSize(m_editor->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->lineNumberAreaPaintEvent(event); }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_editor->gutterClicked(event->pos());
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        setCursor(m_editor->foldIconAt(event->pos()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }
    void enterEvent(QEnterEvent *event) override
    {
        m_editor->setGutterHover(true);
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override
    {
        m_editor->setGutterHover(false);
        QWidget::leaveEvent(event);
    }

private:
    CodeEditor *m_editor;
};

constexpr int kMaxBracketBlocks = 6000;

QString relativeTime(qint64 secs)
{
    const qint64 d = QDateTime::currentSecsSinceEpoch() - secs;
    auto n = [](qint64 v, const char *unit) { return QCoreApplication::translate("CodeEditor", unit, nullptr, int(v)); };
    if (d < 60)
        return QCoreApplication::translate("CodeEditor", "just now");
    if (d < 3600)
        return n(d / 60, "%n min ago");
    if (d < 86400)
        return n(d / 3600, "%n hr ago");
    if (d < 86400 * 30)
        return n(d / 86400, "%n day(s) ago");
    if (d < 86400 * 365)
        return n(d / (86400 * 30), "%n month(s) ago");
    return n(d / (86400 * 365), "%n yr ago");
}
constexpr int kFoldWidth = 16;
bool isImportLine(const QTextBlock &b) { return b.text().startsWith(QLatin1String("import ")); }

// Per-block fold flag (a QTextBlock owns its user data).
struct FoldData : QTextBlockUserData {
    bool folded = false;
};

bool isOpeningBracket(QChar c)
{
    return c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{');
}

bool isBracket(QChar c)
{
    return isOpeningBracket(c) || c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}');
}

QChar partnerOf(QChar c)
{
    switch (c.unicode()) {
    case '(': return QLatin1Char(')');
    case ')': return QLatin1Char('(');
    case '[': return QLatin1Char(']');
    case ']': return QLatin1Char('[');
    case '{': return QLatin1Char('}');
    default: return QLatin1Char('{');
    }
}

// One flag per character: true inside a string or comment (as classified by the syntax highlighter).
// Empty when the block has no highlighting, meaning "all code".
QVector<bool> protectedMask(const QTextBlock &block)
{
    QVector<bool> mask;
    for (const QTextLayout::FormatRange &r : block.layout()->formats()) {
        const QVariant role = r.format.property(kTokenRoleProperty);
        if (!role.isValid())
            continue;
        const int v = role.toInt();
        if (v != int(TokenRole::String) && v != int(TokenRole::Comment))
            continue;
        if (mask.isEmpty())
            mask.fill(false, block.length());
        for (int i = r.start; i < r.start + r.length && i < mask.size(); ++i)
            mask[i] = true;
    }
    return mask;
}

bool inCommentAt(const QTextDocument *doc, int position)
{
    if (position <= 0)
        return false;
    const QTextBlock block = doc->findBlock(position - 1);
    const int i = position - 1 - block.position();
    for (const QTextLayout::FormatRange &r : block.layout()->formats())
        if (r.format.property(kTokenRoleProperty).toInt() == int(TokenRole::Comment) && r.format.property(kTokenRoleProperty).isValid() &&
            i >= r.start && i < r.start + r.length)
            return true;
    return false;
}

// Web languages complete inside strings too: import paths, class names, CSS values, attribute values.
bool completesInStrings(const QString &lang)
{
    return lang == QLatin1String("TypeScript") || lang == QLatin1String("JavaScript") || lang == QLatin1String("HTML") ||
           lang == QLatin1String("CSS") || lang == QLatin1String("JSON");
}

bool isBlankBefore(const QString &text, int pos)
{
    for (int i = 0; i < pos; ++i)
        if (!text.at(i).isSpace())
            return false;
    return true;
}

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    m_lineArea = new LineNumberArea(this);
    m_minimap = new MiniMap(this);
    m_matchTimer = new QTimer(this);
    m_matchTimer->setSingleShot(true);
    m_matchTimer->setInterval(150);
    connect(m_matchTimer, &QTimer::timeout, this, &CodeEditor::recomputeMatches);

    setFrameShape(QFrame::NoFrame);
    setStyleSheet(QStringLiteral("QPlainTextEdit { border: none; border-radius: 0; }")); // no focus ring inside the island
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    viewport()->setMouseTracking(true);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::refreshSelections);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { if (!m_blame.isEmpty()) viewport()->update(); });
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        updateGuideScope();
        if (m_foldedCount > 0)
            m_foldTimer->start();
        if (!m_term.isEmpty())
            m_matchTimer->start();
        if (m_hasBase)
            m_diffTimer->start();
    });
    m_foldTimer = new QTimer(this);
    m_foldTimer->setSingleShot(true);
    m_foldTimer->setInterval(0);
    connect(m_foldTimer, &QTimer::timeout, this, &CodeEditor::applyFolds);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        // A caret inside hidden text (search, go to line, undo) reveals it.
        if (m_foldedCount == 0 || textCursor().block().isVisible())
            return;
        // Nested folds can hide the same line several times over: open them all, innermost first.
        for (int guard = 0; guard < 64 && !textCursor().block().isVisible(); ++guard) {
            const QTextBlock h = foldHeaderFor(textCursor().block(), true);
            if (!h.isValid())
                break;
            setFolded(h, false);
            applyFolds();
        }
        ensureCursorVisible();
    });
    m_diffTimer = new QTimer(this);
    m_diffTimer->setSingleShot(true);
    m_diffTimer->setInterval(200);
    connect(m_diffTimer, &QTimer::timeout, this, &CodeEditor::recomputeDiff);

    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &CodeEditor::positionMinimap);
    applyTheme();
    applySettings();
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &CodeEditor::applySettings);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &CodeEditor::applyTheme);
    updateLineNumberAreaWidth();
    refreshSelections();
}

void CodeEditor::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_gutterBg = t.gutterBg;
    m_gutterFg = t.gutterFg;
    m_gutterActive = t.gutterActiveFg;
    m_currentLine = t.currentLine;
    m_border = t.border;
    m_markAdded = t.gitAdded;
    m_markModified = t.accent;
    m_markDeleted = t.gitDeleted;
    m_diffAddBg = t.diffAddBg;
    m_diffDelBg = t.diffDelBg;
    m_guide = t.gutterFg;
    m_guide.setAlpha(t.dark ? 150 : 130);
    m_guideActive = t.textMuted;
    m_guideActive.setAlpha(230);
    m_bracketOk = QColor(t.accent.red(), t.accent.green(), t.accent.blue(), t.dark ? 80 : 60);
    m_bracketBad = QColor(t.gitConflict.red(), t.gitConflict.green(), t.gitConflict.blue(), t.dark ? 110 : 80);
    m_matchBg = t.dark ? QColor(QStringLiteral("#614d1f")) : QColor(QStringLiteral("#f5e08a"));
    QPalette p = palette();
    p.setColor(QPalette::Base, t.editorBg);
    p.setColor(QPalette::Text, t.editorFg);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.editorFg);
    setPalette(p);
    m_lineArea->update();
    m_minimap->update();
    refreshSelections();
}

void CodeEditor::attachDocument(QTextDocument *doc)
{
    setDocument(doc);
    // Text edits and highlighting both surface as contentsChanged.
    connect(doc, &QTextDocument::contentsChanged, m_minimap, &MiniMap::invalidate);
    m_blameBlocks = doc->blockCount();
    disconnect(m_blameConn);
    m_blameConn = connect(doc, &QTextDocument::contentsChange, this, &CodeEditor::trackLineEdit);
    applySettings();
    updateLineNumberAreaWidth();
    updateGuideScope();
    m_minimap->invalidate();
}

void CodeEditor::applySettings()
{
    auto &s = SettingsManager::instance();
    const QFont f = s.editorFont();
    setFont(f);
    document()->setDefaultFont(f); // setFont() does not reach the document when the widget font is unchanged
    setTabStopDistance(QFontMetricsF(f).horizontalAdvance(QLatin1Char(' ')) * s.tabSize());
    setLineWrapMode(s.wordWrap() ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_indentGuides = s.indentGuides();
    m_stickyScroll = s.stickyScroll();
    m_blameInline = s.blameInline();
    m_blameGutter = s.blameGutter();
    m_showMinimap = s.showMinimap();
    updateGuideScope();
    viewport()->update();
    updateLineNumberAreaWidth();
    m_lineArea->update();
}

// --- Line numbers ----------------------------------------------------------

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    digits = qMax(digits, 3);
    return blameWidth() + 14 + kFoldWidth + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, minimapWidth(), 0);
    positionMinimap();
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineArea->scroll(0, dy);
    else
        m_lineArea->update(0, rect.y(), m_lineArea->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void CodeEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    const QRect cr = contentsRect();
    m_lineArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    positionMinimap();
}

int CodeEditor::minimapWidth() const
{
    return m_showMinimap ? MiniMap::kWidth : 0;
}

void CodeEditor::positionMinimap()
{
    m_minimap->setVisible(m_showMinimap);
    if (!m_showMinimap)
        return;
    // Sits between the text and the vertical scroll bar.
    const QRect vp = viewport()->geometry();
    m_minimap->setGeometry(vp.right() + 1, vp.top(), MiniMap::kWidth, vp.height());
}

void CodeEditor::visibleBlockRange(int *first, int *last) const
{
    *first = cursorForPosition(QPoint(0, 0)).blockNumber();
    *last = cursorForPosition(QPoint(0, viewport()->height() - 1)).blockNumber();
}

// Scroll so that `blockNumber` is in the middle of the viewport. The scroll bar counts laid-out lines, so
// wrapped blocks count once per screen line and hidden (folded) blocks not at all.
void CodeEditor::scrollBlockToCenter(int blockNumber)
{
    int lines = 0;
    for (QTextBlock b = document()->begin(); b.isValid() && b.blockNumber() < blockNumber; b = b.next())
        lines += b.isVisible() ? qMax(1, b.lineCount()) : 0;
    const int perPage = qMax(1, viewport()->height() / qMax(1, fontMetrics().lineSpacing()));
    verticalScrollBar()->setValue(qMax(0, lines - perPage / 2));
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_lineArea);
    painter.fillRect(event->rect(), m_gutterBg);
    painter.setPen(m_border);
    painter.drawLine(m_lineArea->width() - 1, event->rect().top(), m_lineArea->width() - 1, event->rect().bottom());

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    const QColor bookmarkColor = Theme::byName(SettingsManager::instance().theme()).accent;

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const auto diag = m_diagnosticLines.constFind(number);
            painter.setPen(diag != m_diagnosticLines.constEnd() && diag.value() <= LspDiagnostic::Warning
                               ? diagnosticColor(diag.value())
                               : (number == current ? m_gutterActive : m_gutterFg));
            painter.drawText(blameWidth(), top, m_lineArea->width() - blameWidth() - 8 - kFoldWidth, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
            if (!m_bookmarks.isEmpty() && m_bookmarks.contains(number)) {
                QPainterPath flag;
                const qreal bx = blameWidth() + 6, by = top + (fontMetrics().height() - 11) / 2.0;
                flag.moveTo(bx, by);
                flag.lineTo(bx + 8, by);
                flag.lineTo(bx + 8, by + 11);
                flag.lineTo(bx + 4, by + 8);
                flag.lineTo(bx, by + 11);
                flag.closeSubpath();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(bookmarkColor);
                painter.drawPath(flag);
                painter.setRenderHint(QPainter::Antialiasing, false);
            }
            if (blameWidth() > 0 && number < m_blame.size()) {
                const GitBlameLine &bl = m_blame.at(number);
                const bool first = number == 0 || m_blame.at(number - 1).hash != bl.hash;
                if (bl.committed()) {
                    const QColor stripe = QColor::fromHsl(int(qHash(bl.hash) % 360), 110, m_gutterBg.lightness() < 128 ? 120 : 160);
                    painter.fillRect(blameWidth() - 4, top, 2, bottom - top, stripe);
                }
                if (first) {
                    QFont bf = font();
                    bf.setPointSizeF(qMax(7.0, bf.pointSizeF() - 1.5));
                    painter.setFont(bf);
                    painter.setPen(m_gutterFg);
                    const QString label = bl.committed() ? bl.author + QStringLiteral("  ") + relativeTime(bl.time) : tr("You  uncommitted");
                    painter.drawText(QRect(10, top, blameWidth() - 18, bottom - top), Qt::AlignVCenter | Qt::AlignLeft,
                                     QFontMetrics(bf).elidedText(label, Qt::ElideRight, blameWidth() - 18));
                    painter.setFont(font());
                }
            }
            const bool folded = isFolded(block);
            if (folded || ((m_gutterHover || isImportLine(block)) && isFoldable(block))) {
                const QColor c = folded || number == current ? m_gutterActive : m_gutterFg;
                painter.drawPixmap(m_lineArea->width() - kFoldWidth + 1, top + (fontMetrics().height() - 12) / 2,
                                   Icons::pixmap(folded ? QStringLiteral(":/new-icons/chevron-right.svg")
                                                        : QStringLiteral(":/new-icons/chevron-down.svg"),
                                                 c, 12));
            }
            if (m_hasBase) {
                const int h = bottom - top;
                const auto it = m_hunkAtLine.constFind(number);
                if (it != m_hunkAtLine.constEnd())
                    painter.fillRect(0, top, 3, h, m_hunks.at(it.value()).isAdded() ? m_markAdded : m_markModified);
                // Removed lines are marked with a small triangle on the boundary where they used to be.
                auto triangle = [&](int y) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(m_markDeleted);
                    const QPoint pts[3] = {QPoint(0, y - 4), QPoint(6, y), QPoint(0, y + 4)};
                    painter.drawPolygon(pts, 3);
                };
                if (m_deletedAt.contains(number))
                    triangle(top);
                if (number == blockCount() - 1 && m_deletedAt.contains(number + 1))
                    triangle(bottom);
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

// --- Highlights ------------------------------------------------------------

void CodeEditor::refreshSelections()
{
    updateGuideScope();
    QList<QTextEdit::ExtraSelection> extra;

    QTextEdit::ExtraSelection line;
    line.format.setBackground(m_currentLine);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    extra.append(line);

    // Only decorate the matches near the viewport to keep this cheap on big files.
    const int first = cursorForPosition(QPoint(0, 0)).position();
    const int last = cursorForPosition(QPoint(viewport()->width(), viewport()->height())).position();
    for (const auto &m : std::as_const(m_matches)) {
        if (m.second < first)
            continue;
        if (m.first > last)
            break;
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(m_matchBg);
        sel.cursor = QTextCursor(document());
        sel.cursor.setPosition(m.first);
        sel.cursor.setPosition(m.second, QTextCursor::KeepAnchor);
        extra.append(sel);
    }
    appendBracketSelections(extra);
    for (int i = 0; i < m_diagnostics.size() && i < 400; ++i) {
        const LspDiagnostic &d = m_diagnostics.at(i);
        const QPair<int, int> r = diagnosticRange(d);
        if (r.first < 0)
            continue;
        QTextEdit::ExtraSelection sel;
        sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        sel.format.setUnderlineColor(diagnosticColor(d.severity));
        sel.cursor = QTextCursor(document());
        sel.cursor.setPosition(r.first);
        sel.cursor.setPosition(r.second, QTextCursor::KeepAnchor);
        extra.append(sel);
    }
    if (!m_unusedImports.isEmpty()) {
        const QColor muted = Theme::byName(SettingsManager::instance().theme()).textMuted;
        for (const QTextCursor &cur : std::as_const(m_unusedImports)) {
            QTextEdit::ExtraSelection sel;
            sel.format.setForeground(muted);
            sel.cursor = cur;
            extra.append(sel);
        }
    }
    if (m_link.first >= 0) {
        QTextEdit::ExtraSelection sel;
        sel.format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        sel.format.setForeground(Theme::byName(SettingsManager::instance().theme()).accent);
        sel.cursor = QTextCursor(document());
        sel.cursor.setPosition(m_link.first);
        sel.cursor.setPosition(m_link.second, QTextCursor::KeepAnchor);
        extra.append(sel);
    }
    setExtraSelections(extra);
    m_lineArea->update();
}

// --- Language server diagnostics ---------------------------------------------------------------

QColor CodeEditor::diagnosticColor(int severity) const
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    switch (severity) {
    case LspDiagnostic::Error: return t.gitConflict;
    case LspDiagnostic::Warning: return t.gitModified;
    case LspDiagnostic::Information: return t.accent;
    default: return t.textMuted;
    }
}

QPair<int, int> CodeEditor::diagnosticRange(const LspDiagnostic &d) const
{
    const QTextBlock a = document()->findBlockByNumber(d.startLine);
    if (!a.isValid())
        return {-1, -1};
    const QTextBlock b = document()->findBlockByNumber(d.endLine);
    int start = a.position() + qMin(d.startColumn, qMax(0, a.length() - 1));
    int end = b.isValid() ? b.position() + qMin(d.endColumn, qMax(0, b.length() - 1)) : start;
    if (end <= start) { // empty range: underline the character it points at (or the one before at line end)
        const int last = a.position() + qMax(0, a.length() - 1);
        if (start < last)
            end = start + 1;
        else if (start > a.position())
            --start, end = start + 1;
        else
            return {-1, -1};
    }
    return {start, end};
}

void CodeEditor::setDiagnostics(const QVector<LspDiagnostic> &diagnostics)
{
    m_diagnostics = diagnostics;
    m_diagnosticLines.clear();
    for (const LspDiagnostic &d : std::as_const(m_diagnostics)) {
        const auto it = m_diagnosticLines.constFind(d.startLine);
        if (it == m_diagnosticLines.constEnd() || d.severity < it.value())
            m_diagnosticLines.insert(d.startLine, d.severity);
    }
    refreshSelections();
    m_lineArea->update();
}

namespace {
bool isIdentifierChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }
}

bool CodeEditor::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QTextCursor at = cursorForPosition(help->pos());
        const int pos = at.position();
        QStringList lines;
        for (const LspDiagnostic &d : std::as_const(m_diagnostics)) {
            const QPair<int, int> r = diagnosticRange(d);
            if (r.first < 0 || pos < r.first || pos > r.second)
                continue;
            QString text = d.message;
            if (!d.source.isEmpty() || !d.code.isEmpty())
                text += QStringLiteral("  [%1]").arg(d.source.isEmpty() ? d.code : (d.code.isEmpty() ? d.source : d.source + QLatin1Char(' ') + d.code));
            lines << text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        }
        m_hoverDiagnostics = lines.isEmpty() ? QString() : QStringLiteral("<div style='white-space:pre'>") + lines.join(QStringLiteral("<hr>")) + QStringLiteral("</div>");
        m_hoverPos = -1;
        if (m_hoverDiagnostics.isEmpty() && !m_unusedImports.isEmpty() && at.positionInBlock() < at.block().text().size() &&
            unusedImportLines().contains(at.blockNumber())) {
            QToolTip::hideText();
            if (m_hoverPopup)
                m_hoverPopup->close();
            m_hoverPopup = new HoverPopup(this);
            connect(m_hoverPopup, &HoverPopup::linkActivated, this, &CodeEditor::removeUnusedImportsRequested);
            const int n = unusedImportLines().size();
            m_hoverPopup->showHtml(tr("<b>Unused import</b><br>Nothing in this file uses it. "
                                      "<a href='remove'>Remove unused imports…</a> (%n in this file)", nullptr, n),
                                   help->globalPos());
            return true;
        }
        // Only ask the server about words, and only when the mouse is over the text of the line.
        const QString text = at.block().text();
        const int col = at.positionInBlock();
        const QRectF lineRect = blockBoundingGeometry(at.block()).translated(contentOffset());
        const bool onWord = col < text.size() && isIdentifierChar(text.at(col)) && help->pos().y() < lineRect.bottom();
        if (onWord) {
            m_hoverPos = pos;
            m_hoverGlobal = help->globalPos();
            emit hoverRequested(at.blockNumber(), col);
        }
        if (!m_hoverDiagnostics.isEmpty()) {
            QToolTip::showText(help->globalPos(), m_hoverDiagnostics, viewport());
            return true;
        }
        if (onWord)
            return true; // the server's answer may still show a tooltip
        QToolTip::hideText();
    }
    return QPlainTextEdit::viewportEvent(event);
}

void CodeEditor::setUnusedImports(const QVector<int> &lines)
{
    QList<QTextCursor> cursors;
    for (const int n : lines) {
        const QTextBlock b = document()->findBlockByNumber(n);
        if (!b.isValid() || !b.text().trimmed().startsWith(QLatin1String("import ")))
            continue;
        QTextCursor c(b);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursors.append(c);
    }
    const bool changed = cursors.size() != m_unusedImports.size() || !cursors.isEmpty() || !m_unusedImports.isEmpty();
    m_unusedImports = cursors;
    if (changed)
        refreshSelections();
}

QVector<int> CodeEditor::unusedImportLines() const
{
    QVector<int> out;
    for (const QTextCursor &c : m_unusedImports) {
        const QTextBlock b = document()->findBlock(c.selectionStart());
        if (b.isValid() && b.text().trimmed().startsWith(QLatin1String("import ")) && !out.contains(b.blockNumber()))
            out.append(b.blockNumber());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void CodeEditor::removeLines(const QVector<int> &lines)
{
    if (isReadOnly() || lines.isEmpty())
        return;
    QVector<int> sorted = lines;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    QTextCursor c(document());
    c.beginEditBlock();
    for (const int n : std::as_const(sorted)) {
        const QTextBlock b = document()->findBlockByNumber(n);
        if (!b.isValid())
            continue;
        c.setPosition(b.position());
        if (b.next().isValid()) {
            c.setPosition(b.next().position(), QTextCursor::KeepAnchor);
        } else { // last line of the file: take the newline before it instead
            c.setPosition(qMax(0, b.position() - 1));
            c.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
        }
        c.removeSelectedText();
    }
    c.endEditBlock();
}

void CodeEditor::showHover(const QString &markdown)
{
    if (m_hoverPos < 0 || markdown.isEmpty() || !viewport()->underMouse())
        return;
    if (cursorForPosition(viewport()->mapFromGlobal(QCursor::pos())).position() != m_hoverPos)
        return; // the mouse moved on
    constexpr int kMaxChars = 1800;
    QString md = markdown;
    if (md.size() > kMaxChars)
        md = md.left(kMaxChars) + QStringLiteral("\n\n…");
    QTextDocument doc;
    doc.setMarkdown(md);
    QString html = doc.toHtml();
    const int bodyStart = html.indexOf(QStringLiteral("<body"));
    if (bodyStart >= 0) // keep the fonts of the tooltip, drop the page styling
        html = QStringLiteral("<div>") + html.mid(html.indexOf(QLatin1Char('>'), bodyStart) + 1);
    html.remove(QStringLiteral("</body></html>"));
    html = QStringLiteral("<style>pre,code{white-space:pre-wrap}</style>") + html + QStringLiteral("</div>");
    if (!m_hoverDiagnostics.isEmpty())
        html = m_hoverDiagnostics + QStringLiteral("<hr>") + html;
    QToolTip::hideText();
    if (m_hoverPopup)
        m_hoverPopup->close();
    m_hoverPopup = new HoverPopup(this);
    m_hoverPopup->showHtml(html, m_hoverGlobal);
}

// --- Code folding -------------------------------------------------------------

void CodeEditor::setGutterHover(bool on)
{
    m_gutterHover = on;
    m_lineArea->update();
}

bool CodeEditor::isFolded(const QTextBlock &block) const
{
    auto *d = static_cast<FoldData *>(block.userData());
    return d && d->folded;
}

void CodeEditor::setFolded(const QTextBlock &block, bool folded)
{
    auto *d = static_cast<FoldData *>(block.userData());
    if (!d) {
        if (!folded)
            return;
        d = new FoldData;
        QTextBlock(block).setUserData(d); // the handle is a cheap copy; the block takes ownership
    }
    d->folded = folded;
}

// The last opening bracket of the line that is not closed on the same line (position in the document), or -1.
int CodeEditor::unmatchedOpener(const QTextBlock &block) const
{
    const QString text = block.text();
    const QVector<bool> mask = protectedMask(block);
    QList<int> stack;
    for (int i = 0; i < text.size(); ++i) {
        if (!mask.isEmpty() && mask.at(i))
            continue;
        const QChar c = text.at(i);
        if (isOpeningBracket(c)) {
            stack.append(i);
        } else if (isBracket(c) && !stack.isEmpty() && text.at(stack.last()) == partnerOf(c)) {
            stack.removeLast();
        }
    }
    return stack.isEmpty() ? -1 : block.position() + stack.last();
}

// The import list is one fold: `header` is its first `import` line and the region runs to the last import of the
// run (blank lines between imports are part of it). Needs at least two imports.
int CodeEditor::importRunEnd(const QTextBlock &header) const
{
    if (!isImportLine(header))
        return -1;
    int blanks = 0;
    for (QTextBlock p = header.previous(); p.isValid() && blanks < 50; p = p.previous(), ++blanks) {
        if (isImportLine(p))
            return -1; // not the first import
        if (!p.text().trimmed().isEmpty())
            break;
    }
    int last = -1;
    int n = 0;
    for (QTextBlock b = header.next(); b.isValid() && n < 5000; b = b.next(), ++n) {
        if (isImportLine(b))
            last = b.blockNumber();
        else if (!b.text().trimmed().isEmpty())
            break;
    }
    return last;
}

bool CodeEditor::isFoldable(const QTextBlock &block) const
{
    if (!block.isValid() || !block.next().isValid())
        return false;
    if (importRunEnd(block) >= 0)
        return true;
    const QString text = block.text();
    if (text.trimmed().isEmpty())
        return false;

    const int opener = unmatchedOpener(block);
    if (opener >= 0) {
        // `foo(` directly followed by its closer has nothing to fold.
        const QString next = block.next().text().trimmed();
        return !(next.startsWith(QLatin1Char(')')) || next.startsWith(QLatin1Char(']')) || next.startsWith(QLatin1Char('}')));
    }
    const QString t = text.trimmed();
    if (t.startsWith(QStringLiteral("/*")) && !t.contains(QStringLiteral("*/")) && block.userState() > 0)
        return true;

    bool blank = false;
    const int own = indentDepth(block);
    QTextBlock b = block.next();
    for (int n = 0; b.isValid() && n < 200; b = b.next(), ++n) {
        const int d = indentDepth(b, &blank);
        if (!blank)
            return d > own;
    }
    return false;
}

int CodeEditor::foldEnd(const QTextBlock &header) const
{
    const int first = header.blockNumber();
    if (const int imports = importRunEnd(header); imports >= 0)
        return imports;
    const int opener = unmatchedOpener(header);
    if (opener >= 0) {
        const int match = findMatchingBracket(opener);
        if (match >= 0) {
            const int end = document()->findBlock(match).blockNumber() - 1;
            return end > first ? end : -1;
        }
    }
    const QString t = header.text().trimmed();
    if (t.startsWith(QStringLiteral("/*")) && !t.contains(QStringLiteral("*/")) && header.userState() > 0) {
        QTextBlock b = header.next();
        for (; b.isValid(); b = b.next())
            if (b.userState() <= 0)
                return b.blockNumber(); // the line with the closing */ is hidden too
        return -1;
    }

    bool blank = false;
    const int own = indentDepth(header);
    int last = -1;
    QTextBlock b = header.next();
    for (; b.isValid(); b = b.next()) {
        const int d = indentDepth(b, &blank);
        if (blank)
            continue;
        if (d <= own)
            break;
        last = b.blockNumber();
    }
    return last;
}

// The folded block hiding `block` (foldedOnly), or the nearest line above whose region contains it.
QTextBlock CodeEditor::foldHeaderFor(const QTextBlock &block, bool foldedOnly) const
{
    const int n = block.blockNumber();
    int scanned = 0;
    for (QTextBlock b = block.previous(); b.isValid() && scanned < 20000; b = b.previous(), ++scanned) {
        if (foldedOnly && !isFolded(b))
            continue;
        if (!foldedOnly && !isFoldable(b))
            continue;
        if (foldEnd(b) >= n)
            return b;
    }
    return QTextBlock();
}

// Recomputes which blocks are hidden from the fold flags; also drops flags that no longer cover anything.
void CodeEditor::applyFolds()
{
    m_foldTimer->stop();
    QTextDocument *doc = document();
    int hideUntil = -1;
    int folded = 0;
    int firstChanged = -1, lastChanged = -1;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const int n = b.blockNumber();
        bool visible = n > hideUntil;
        if (visible && isFolded(b)) {
            const int end = isFoldable(b) ? foldEnd(b) : -1; // the header may have been edited away
            if (end > n) {
                hideUntil = end;
                ++folded;
            } else {
                setFolded(b, false);
            }
        }
        if (b.isVisible() != visible) {
            b.setVisible(visible);
            b.setLineCount(visible ? qMax(1, b.layout() ? b.layout()->lineCount() : 1) : 0);
            if (firstChanged < 0)
                firstChanged = b.position();
            lastChanged = b.position() + b.length();
        }
    }
    m_foldedCount = folded;
    m_minimap->invalidate();
    if (firstChanged >= 0) {
        doc->markContentsDirty(firstChanged, lastChanged - firstChanged);
        if (auto *layout = qobject_cast<QPlainTextDocumentLayout *>(doc->documentLayout()))
            layout->requestUpdate();
    }
    updateLineNumberAreaWidth();
    viewport()->update();
    m_lineArea->update();
}

void CodeEditor::toggleFoldAt(int blockNumber)
{
    const QTextBlock b = document()->findBlockByNumber(blockNumber);
    if (!b.isValid())
        return;
    const bool fold = !isFolded(b);
    if (fold && !isFoldable(b))
        return;
    setFolded(b, fold);
    if (isImportLine(b) && importRunEnd(b) >= 0)
        emit importsFoldedChanged(fold);
    if (fold) {
        // Keep the caret out of the region that is about to disappear.
        const int end = foldEnd(b);
        const int cur = textCursor().blockNumber();
        if (end > blockNumber && cur > blockNumber && cur <= end) {
            QTextCursor c(b);
            c.movePosition(QTextCursor::EndOfBlock);
            setTextCursor(c);
        }
    }
    applyFolds();
}

void CodeEditor::setImportsFolded(bool folded)
{
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (isImportLine(b)) {
            if (importRunEnd(b) >= 0 && isFolded(b) != folded)
                toggleFoldAt(b.blockNumber());
            return;
        }
}

void CodeEditor::foldCurrent()
{
    QTextBlock b = textCursor().block();
    // Prefer the caret's own line when it can fold and is not already folded, else the enclosing region.
    if (!isFoldable(b) || isFolded(b)) {
        QTextBlock h = foldHeaderFor(b, false);
        // Skip enclosing regions that are already folded (only possible for the visible outermost one).
        while (h.isValid() && isFolded(h))
            h = foldHeaderFor(h, false);
        if (!h.isValid())
            return;
        b = h;
    }
    toggleFoldAt(b.blockNumber());
}

void CodeEditor::unfoldCurrent()
{
    QTextBlock b = textCursor().block();
    if (!isFolded(b))
        b = foldHeaderFor(b, true);
    if (b.isValid() && isFolded(b))
        toggleFoldAt(b.blockNumber());
}

void CodeEditor::foldAll()
{
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (isFoldable(b))
            setFolded(b, true);
    const QTextBlock cur = textCursor().block();
    applyFolds();
    if (!cur.isVisible()) {
        if (QTextBlock h = foldHeaderFor(cur, true); h.isValid()) {
            QTextCursor c(h);
            c.movePosition(QTextCursor::EndOfBlock);
            setTextCursor(c);
        }
    }
}

void CodeEditor::unfoldAll()
{
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (isFolded(b))
            setFolded(b, false);
    applyFolds();
}

// The inline "…" that stands in for the hidden lines after a folded header.
void CodeEditor::paintFoldMarkers()
{
    m_foldPills.clear();
    if (m_foldedCount == 0)
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const qreal x0 = contentOffset().x() + document()->documentMargin();
    const int lineH = fontMetrics().height();
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= viewport()->height()) {
        const int h = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && h > 0) {
            if (isFolded(block) && block.layout() && block.layout()->lineCount() > 0) {
                const QTextLine line = block.layout()->lineAt(block.layout()->lineCount() - 1);
                const int x = qRound(x0 + line.naturalTextRect().right()) + 8;
                const int y = top + qRound(line.y()) + (lineH - 14) / 2;
                const QRect pill(x, y, 26, 14);
                QColor bg = m_gutterFg;
                bg.setAlpha(90);
                p.setPen(Qt::NoPen);
                p.setBrush(bg);
                p.drawRoundedRect(pill, 4, 4);
                p.setBrush(m_gutterActive);
                for (int i = 0; i < 3; ++i)
                    p.drawEllipse(QPointF(pill.left() + 7 + i * 6, pill.center().y() + 0.5), 1.4, 1.4);
                m_foldPills.append({pill, block.blockNumber()});
            }
            top += h;
        }
        block = block.next();
    }
}

// --- Indent guides -------------------------------------------------------------

int CodeEditor::indentDepth(const QTextBlock &block, bool *blank) const
{
    const QString text = block.text();
    const int tab = qMax(1, SettingsManager::instance().tabSize());
    int cols = 0, i = 0;
    for (; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char(' '))
            ++cols;
        else if (text.at(i) == QLatin1Char('\t'))
            cols += tab - cols % tab;
        else
            break;
    }
    if (blank)
        *blank = i == text.size();
    return (cols + tab - 1) / tab;
}

int CodeEditor::effectiveDepth(const QTextBlock &block) const
{
    bool blank = false;
    const int own = indentDepth(block, &blank);
    if (!blank)
        return own;
    // An empty line inside a block keeps the guides of the code around it.
    int up = 0, down = 0;
    QTextBlock b = block.previous();
    for (int n = 0; b.isValid() && n < 60; b = b.previous(), ++n) {
        bool bl = false;
        const int d = indentDepth(b, &bl);
        if (!bl) {
            up = d;
            break;
        }
    }
    b = block.next();
    for (int n = 0; b.isValid() && n < 60; b = b.next(), ++n) {
        bool bl = false;
        const int d = indentDepth(b, &bl);
        if (!bl) {
            down = d;
            break;
        }
    }
    return qMin(up, down);
}

// The guide belonging to the caret's line is drawn brighter along the whole scope it delimits.
void CodeEditor::updateGuideScope()
{
    m_guideScope = {};
    if (!m_indentGuides)
        return;
    const QTextBlock cur = textCursor().block();
    const int depth = effectiveDepth(cur);
    if (depth < 1)
        return;
    auto inScope = [&](const QTextBlock &b) {
        bool blank = false;
        const int d = indentDepth(b, &blank);
        return blank || d >= depth;
    };
    int first = cur.blockNumber(), last = first;
    int n = 0;
    for (QTextBlock b = cur.previous(); b.isValid() && n < 4000 && inScope(b); b = b.previous(), ++n)
        first = b.blockNumber();
    n = 0;
    for (QTextBlock b = cur.next(); b.isValid() && n < 4000 && inScope(b); b = b.next(), ++n)
        last = b.blockNumber();
    m_guideScope.level = depth - 1;
    m_guideScope.first = first;
    m_guideScope.last = last;
    viewport()->update();
}

void CodeEditor::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    if (m_indentGuides)
        paintIndentGuides();
    paintFoldMarkers();
    paintStickyScroll();
    paintBlameAnnotation();
}

QList<int> CodeEditor::stickyLines() const
{
    QList<int> rows;
    if (!m_stickyScroll || m_language.isEmpty())
        return rows;
    const QTextBlock first = firstVisibleBlock();
    if (!first.isValid() || first.blockNumber() == 0)
        return rows;
    // The pinned rows cover the first lines, so the scopes that matter are those of the line under them.
    for (int pass = 0; pass < 3; ++pass) {
        const int probe = first.blockNumber() + rows.size();
        QList<int> next;
        for (const Breadcrumbs::Crumb &c : Breadcrumbs::symbolChain(document(), probe, m_language))
            if (c.line >= 0 && c.line < probe)
                next.append(c.line);
        if (next.size() > 3)
            next = next.mid(next.size() - 3);
        if (next.size() == rows.size()) {
            rows = next;
            break;
        }
        rows = next;
    }
    return rows;
}

void CodeEditor::paintStickyScroll()
{
    m_stickyRows.clear();
    const QList<int> rows = stickyLines();
    if (rows.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPainter p(viewport());
    p.setFont(font());
    const int lh = qRound(blockBoundingRect(firstVisibleBlock()).height());
    const int h = lh * rows.size();
    QColor bg = t.editorBg;
    p.fillRect(QRect(0, 0, viewport()->width(), h), bg);
    p.setPen(t.border);
    p.drawLine(0, h - 1, viewport()->width(), h - 1);
    const int x0 = qRound(document()->documentMargin());
    for (int i = 0; i < rows.size(); ++i) {
        const QTextBlock b = document()->findBlockByNumber(rows.at(i));
        const QRect r(0, i * lh, viewport()->width(), lh);
        p.setPen(t.editorFg);
        const QString text = b.text().trimmed().replace(QLatin1Char('\t'), QLatin1Char(' '));
        p.drawText(r.adjusted(x0, 0, -x0, 0), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font()).elidedText(text, Qt::ElideRight, r.width() - 2 * x0));
        m_stickyRows.append({r, rows.at(i)});
    }
}

void CodeEditor::paintIndentGuides()
{
    QPainter p(viewport());
    const int tab = qMax(1, SettingsManager::instance().tabSize());
    const qreal cw = QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' '));
    const qreal x0 = contentOffset().x() + document()->documentMargin();
    const int bottomLimit = viewport()->height();

    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= bottomLimit) {
        const int h = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && h > 0) {
            const int depth = effectiveDepth(block);
            const bool inScope = block.blockNumber() >= m_guideScope.first && block.blockNumber() <= m_guideScope.last;
            for (int k = 0; k < depth; ++k) {
                const int x = qRound(x0 + k * tab * cw);
                p.setPen(inScope && k == m_guideScope.level ? m_guideActive : m_guide);
                p.drawLine(x, top, x, top + h - 1);
            }
            top += h;
        }
        block = block.next();
    }
}

// --- Bracket matching ---------------------------------------------------------

int CodeEditor::bracketNearCursor() const
{
    const QTextCursor c = textCursor();
    if (c.hasSelection())
        return -1;
    const QTextBlock block = c.block();
    const QString text = block.text();
    const int col = c.positionInBlock();
    // The bracket just before the cursor wins over the one just after it.
    for (int candidate : {col - 1, col}) {
        if (candidate < 0 || candidate >= text.size() || !isBracket(text.at(candidate)))
            continue;
        const QVector<bool> mask = protectedMask(block);
        if (mask.isEmpty() || !mask.at(candidate))
            return block.position() + candidate;
    }
    return -1;
}

int CodeEditor::findMatchingBracket(int pos) const
{
    QTextBlock block = document()->findBlock(pos);
    if (!block.isValid())
        return -1;
    const int col = pos - block.position();
    const QChar ch = block.text().at(col);
    const QChar other = partnerOf(ch);
    const bool forward = isOpeningBracket(ch);
    int depth = 0;
    int blocks = 0;
    for (QTextBlock b = block; b.isValid() && blocks < kMaxBracketBlocks; b = forward ? b.next() : b.previous(), ++blocks) {
        const QString text = b.text();
        const QVector<bool> mask = protectedMask(b);
        int i = forward ? (b == block ? col : 0) : (b == block ? col : text.size() - 1);
        for (; i >= 0 && i < text.size(); i += forward ? 1 : -1) {
            if (!mask.isEmpty() && mask.at(i))
                continue;
            const QChar c = text.at(i);
            if (c == ch) {
                ++depth;
            } else if (c == other && --depth == 0) {
                return b.position() + i;
            }
        }
    }
    return -1;
}

void CodeEditor::appendBracketSelections(QList<QTextEdit::ExtraSelection> &extra) const
{
    const int pos = bracketNearCursor();
    if (pos < 0)
        return;
    const int match = findMatchingBracket(pos);
    auto mark = [&](int at, const QColor &color) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(color);
        sel.format.setFontWeight(QFont::Bold);
        sel.cursor = QTextCursor(document());
        sel.cursor.setPosition(at);
        sel.cursor.setPosition(at + 1, QTextCursor::KeepAnchor);
        extra.append(sel);
    };
    if (match >= 0) {
        mark(pos, m_bracketOk);
        mark(match, m_bracketOk);
    } else {
        mark(pos, m_bracketBad); // unmatched
    }
}

void CodeEditor::gotoMatchingBracket()
{
    const int pos = bracketNearCursor();
    if (pos < 0)
        return;
    const int match = findMatchingBracket(pos);
    if (match < 0)
        return;
    QTextCursor c = textCursor();
    c.setPosition(match + (isOpeningBracket(document()->characterAt(match)) ? 0 : 1));
    setTextCursor(c);
    ensureCursorVisible();
}

// --- Search ----------------------------------------------------------------

void CodeEditor::setSearchTerm(const QString &term, bool caseSensitive)
{
    m_term = term;
    m_caseSensitive = caseSensitive;
    recomputeMatches();
}

void CodeEditor::recomputeMatches()
{
    m_matches.clear();
    if (!m_term.isEmpty()) {
        const QTextDocument::FindFlags flags = m_caseSensitive ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags();
        QTextCursor c(document());
        while (m_matches.size() < 100000) {
            c = document()->find(m_term, c, flags);
            if (c.isNull())
                break;
            m_matches.append({c.selectionStart(), c.selectionEnd()});
        }
    }
    refreshSelections();
    emit searchResultsChanged();
}

int CodeEditor::currentMatchIndex() const
{
    const QTextCursor c = textCursor();
    for (int i = 0; i < m_matches.size(); ++i)
        if (m_matches[i].first == c.selectionStart() && m_matches[i].second == c.selectionEnd())
            return i + 1;
    return 0;
}

bool CodeEditor::findNext(bool backwards)
{
    if (m_term.isEmpty())
        return false;
    QTextDocument::FindFlags flags = m_caseSensitive ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags();
    if (backwards)
        flags |= QTextDocument::FindBackward;

    QTextCursor found = document()->find(m_term, textCursor(), flags);
    if (found.isNull()) {
        // Wrap around.
        QTextCursor start(document());
        if (backwards)
            start.movePosition(QTextCursor::End);
        found = document()->find(m_term, start, flags);
    }
    if (found.isNull())
        return false;
    setTextCursor(found);
    centerCursor();
    emit searchResultsChanged();
    return true;
}

bool CodeEditor::selectionIsMatch() const
{
    const QTextCursor c = textCursor();
    if (!c.hasSelection() || m_term.isEmpty())
        return false;
    return c.selectedText().compare(m_term, m_caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0;
}

bool CodeEditor::replaceCurrent(const QString &replacement)
{
    if (isReadOnly() || m_term.isEmpty())
        return false;
    if (!selectionIsMatch())
        return findNext();
    QTextCursor c = textCursor();
    c.insertText(replacement);
    setTextCursor(c);
    recomputeMatches();
    findNext();
    return true;
}

int CodeEditor::replaceAll(const QString &replacement)
{
    if (isReadOnly() || m_term.isEmpty())
        return 0;
    const QTextDocument::FindFlags flags = m_caseSensitive ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags();
    int count = 0;
    QTextCursor edit(document());
    edit.beginEditBlock();
    QTextCursor c(document());
    while (true) {
        c = document()->find(m_term, c, flags);
        if (c.isNull())
            break;
        c.insertText(replacement);
        ++count;
    }
    edit.endEditBlock();
    recomputeMatches();
    return count;
}

// --- Editing behaviour -----------------------------------------------------

QString CodeEditor::indentUnit() const
{
    auto &s = SettingsManager::instance();
    return s.useSpaces() ? QString(s.tabSize(), QLatin1Char(' ')) : QStringLiteral("\t");
}

namespace {

bool isMarkupLanguage(const QString &lang)
{
    return lang == QLatin1String("TypeScript") || lang == QLatin1String("JavaScript") || lang == QLatin1String("HTML") ||
           lang == QLatin1String("XML");
}

// How the text of a line (up to the caret) leaves an HTML / JSX tag:
//  Attributes - an opening tag is started but not finished (`<motion.a`), so the next line holds its attributes;
//  Children   - the line ends an opening tag (`<div>`, or the lone `>` closing a multi-line tag), so content goes inside.
enum class TagState { None, Attributes, Children };

TagState tagState(const QString &before, bool *closerBelow, const QString &after)
{
    static const QSet<QString> voidTags = {QStringLiteral("br"),   QStringLiteral("hr"),    QStringLiteral("img"),
                                           QStringLiteral("input"), QStringLiteral("meta"),  QStringLiteral("link"),
                                           QStringLiteral("area"),  QStringLiteral("base"),  QStringLiteral("col"),
                                           QStringLiteral("embed"), QStringLiteral("source"), QStringLiteral("track"),
                                           QStringLiteral("wbr"),   QStringLiteral("param")};
    *closerBelow = false;
    const QString t = before.trimmed();
    if (t.isEmpty())
        return TagState::None;

    // Walk the line tracking quotes and braces, remembering the last tag that was opened and whether it got closed.
    int tagStart = -1, braces = 0;
    bool tagOpen = false, selfClosed = false;
    QChar quote;
    for (int i = 0; i < t.size(); ++i) {
        const QChar ch = t.at(i);
        if (!quote.isNull()) {
            if (ch == quote)
                quote = QChar();
            continue;
        }
        if (tagOpen) {
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\''))
                quote = ch;
            else if (ch == QLatin1Char('{'))
                ++braces;
            else if (ch == QLatin1Char('}'))
                --braces;
            else if (ch == QLatin1Char('>') && braces <= 0 && !(i > 0 && t.at(i - 1) == QLatin1Char('='))) {
                selfClosed = i > 0 && t.at(i - 1) == QLatin1Char('/');
                tagOpen = false;
            }
            continue;
        }
        if (ch == QLatin1Char('<')) {
            const QChar prev = i > 0 ? t.at(i - 1) : QLatin1Char(' ');
            const QChar next = i + 1 < t.size() ? t.at(i + 1) : QLatin1Char('>');
            if (next == QLatin1Char('/')) { // `</div>` closes whatever was open on this line
                tagStart = -1;
                continue;
            }
            // `Array<string>` is a generic, `a < b` a comparison and `<!--` a comment: not openers.
            if (prev.isLetterOrNumber() || prev == QLatin1Char('_') || prev == QLatin1Char(')'))
                continue;
            if (!(next.isLetter() || next == QLatin1Char('>')))
                continue;
            tagStart = i;
            tagOpen = true;
            braces = 0;
            selfClosed = false;
        }
    }
    if (tagStart < 0)
        return TagState::None;
    if (tagOpen)
        return t.startsWith(QLatin1Char('<')) && tagStart == 0 ? TagState::Attributes : TagState::None;
    if (selfClosed)
        return TagState::None;
    int e = tagStart + 1;
    while (e < t.size() && (t.at(e).isLetterOrNumber() || t.at(e) == QLatin1Char('.') || t.at(e) == QLatin1Char('-') ||
                            t.at(e) == QLatin1Char(':') || t.at(e) == QLatin1Char('_')))
        ++e;
    if (voidTags.contains(t.mid(tagStart + 1, e - tagStart - 1).toLower()))
        return TagState::None;
    if (!t.endsWith(QLatin1Char('>')))
        return TagState::None;
    *closerBelow = after.trimmed().startsWith(QLatin1String("</"));
    return TagState::Children;
}

} // namespace

void CodeEditor::insertNewlineWithIndent()
{
    QTextCursor c = textCursor();
    const QString line = c.block().text();
    const int col = c.positionInBlock();
    const QString before = line.left(col);
    const QString after = line.mid(col);

    QString indent;
    for (const QChar ch : line) {
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t'))
            indent += ch;
        else
            break;
    }
    if (col < indent.size())
        indent = indent.left(col);

    const QString trimmedBefore = before.trimmed();
    bool tagCloserBelow = false;
    TagState tag = TagState::None;
    if (isMarkupLanguage(m_language)) {
        tag = tagState(before, &tagCloserBelow, after);
        // The lone `>` / `/>` line that finishes a multi-line tag: the line it started on is what children hang from.
        if (tag == TagState::None && trimmedBefore == QLatin1String(">")) {
            tag = TagState::Children;
            tagCloserBelow = after.trimmed().startsWith(QLatin1String("</"));
        }
    }
    const bool opens = tag != TagState::None
        || (!trimmedBefore.isEmpty()
        && (trimmedBefore.endsWith(QLatin1Char('{')) || trimmedBefore.endsWith(QLatin1Char('(')) ||
            trimmedBefore.endsWith(QLatin1Char('[')) ||
            (m_indentAfterColon && trimmedBefore.endsWith(QLatin1Char(':')))));
    c.beginEditBlock();
    c.removeSelectedText();
    if (opens && !after.trimmed().isEmpty() && (tagCloserBelow || after.trimmed().startsWith(QLatin1Char('}')) ||
                                                 after.trimmed().startsWith(QLatin1Char(')')) ||
                                                 after.trimmed().startsWith(QLatin1Char(']')))) {
        // Cursor sits between a bracket pair: open an indented empty line and push the closer below.
        c.insertText(QStringLiteral("\n") + indent + indentUnit());
        const int pos = c.position();
        c.insertText(QStringLiteral("\n") + indent);
        c.setPosition(pos);
    } else {
        c.insertText(QStringLiteral("\n") + indent + (opens ? indentUnit() : QString()));
    }
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
}

void CodeEditor::indentSelection(bool outdent)
{
    QTextCursor c = textCursor();
    const QString unit = indentUnit();
    const int tab = SettingsManager::instance().tabSize();

    QTextCursor start(document());
    start.setPosition(c.selectionStart());
    QTextCursor end(document());
    end.setPosition(c.selectionEnd());
    int firstBlock = start.blockNumber();
    int lastBlock = end.blockNumber();
    // A selection ending at column 0 of a line doesn't include that line.
    if (c.hasSelection() && end.positionInBlock() == 0 && lastBlock > firstBlock)
        --lastBlock;

    c.beginEditBlock();
    for (int b = firstBlock; b <= lastBlock; ++b) {
        QTextCursor lc(document()->findBlockByNumber(b));
        if (outdent) {
            const QString t = lc.block().text();
            int remove = 0;
            if (t.startsWith(QLatin1Char('\t')))
                remove = 1;
            else
                while (remove < tab && remove < t.size() && t.at(remove) == QLatin1Char(' '))
                    ++remove;
            if (remove > 0) {
                lc.movePosition(QTextCursor::StartOfBlock);
                lc.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, remove);
                lc.removeSelectedText();
            }
        } else if (!lc.block().text().isEmpty()) {
            lc.movePosition(QTextCursor::StartOfBlock);
            lc.insertText(unit);
        }
    }
    c.endEditBlock();
}

namespace {
QChar closerFor(QChar open)
{
    switch (open.unicode()) {
    case '(': return QLatin1Char(')');
    case '[': return QLatin1Char(']');
    case '{': return QLatin1Char('}');
    case '"': return QLatin1Char('"');
    case '\'': return QLatin1Char('\'');
    case '`': return QLatin1Char('`');
    default: return QChar();
    }
}
bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }
bool isQuote(QChar c) { return c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`'); }
bool isCloser(QChar c) { return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'); }
}

bool CodeEditor::autoPair(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    const QString text = event->text();
    if (mods != Qt::NoModifier || text.size() != 1 || overwriteMode())
        return false;
    const QChar ch = text[0];
    const QChar close = closerFor(ch);
    if (close.isNull() && !isCloser(ch))
        return false;

    QTextCursor c = textCursor();
    QTextDocument *doc = document();
    const int pos = c.position();
    const QChar next = c.hasSelection() ? QChar() : doc->characterAt(pos); // '\0' at the end of the document
    const QChar prev = pos > 0 ? doc->characterAt(pos - 1) : QChar();

    if (c.hasSelection()) { // wrap the selection and keep it selected
        if (close.isNull())
            return false;
        const int s = c.selectionStart(), e = c.selectionEnd();
        c.beginEditBlock();
        c.setPosition(e);
        c.insertText(QString(close));
        c.setPosition(s);
        c.insertText(QString(ch));
        c.setPosition(s + 1);
        c.setPosition(e + 1, QTextCursor::KeepAnchor);
        c.endEditBlock();
        setTextCursor(c);
        return true;
    }
    // Typing the closer that is already there just steps over it.
    if ((isCloser(ch) || isQuote(ch)) && next == ch) {
        c.movePosition(QTextCursor::Right);
        setTextCursor(c);
        return true;
    }
    if (close.isNull())
        return false;
    // Pair only where it is welcome: not right before a word, not inside strings/comments, and not an
    // apostrophe after a letter (don't) or a Rust lifetime.
    if (isIdentChar(next) || (next == ch))
        return false;
    if (isQuote(ch) && (inStringOrComment(pos) || isIdentChar(prev)))
        return false;
    if (ch == QLatin1Char('\'') && m_language.compare(QLatin1String("rust"), Qt::CaseInsensitive) == 0)
        return false;
    if (inStringOrComment(pos))
        return false;
    c.beginEditBlock();
    c.insertText(QString(ch) + QString(close));
    c.movePosition(QTextCursor::Left);
    c.endEditBlock();
    setTextCursor(c);
    return true;
}

bool CodeEditor::eraseEmptyPair()
{
    QTextCursor c = textCursor();
    if (c.hasSelection() || c.position() == 0)
        return false;
    const QChar prev = document()->characterAt(c.position() - 1);
    const QChar next = document()->characterAt(c.position());
    if (closerFor(prev).isNull() || closerFor(prev) != next)
        return false;
    c.beginEditBlock();
    c.deleteChar();
    c.deletePreviousChar();
    c.endEditBlock();
    setTextCursor(c);
    return true;
}

void CodeEditor::handleBackspaceInIndent(QKeyEvent *event)
{
    QTextCursor c = textCursor();
    const int tab = SettingsManager::instance().tabSize();
    if (!c.hasSelection() && SettingsManager::instance().useSpaces() && c.positionInBlock() > 0) {
        const QString line = c.block().text();
        const int col = c.positionInBlock();
        if (isBlankBefore(line, col) && line.left(col).indexOf(QLatin1Char('\t')) < 0) {
            int remove = col % tab;
            if (remove == 0)
                remove = tab;
            remove = qMin(remove, col);
            c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, remove);
            c.removeSelectedText();
            setTextCursor(c);
            return;
        }
    }
    QPlainTextEdit::keyPressEvent(event);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    if (!isReadOnly()) {
        if (m_completion && m_completion->isVisible() && completionKey(event))
            return;
        if (event->key() == Qt::Key_Space && (event->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier) {
            triggerCompletion();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & ~Qt::KeypadModifier) == Qt::AltModifier) {
            hideCompletion();
            const QTextCursor c = textCursor();
            const QTextBlock a = document()->findBlock(c.selectionStart()), b = document()->findBlock(c.selectionEnd());
            emit codeActionsRequested(a.blockNumber(), c.selectionStart() - a.position(), b.blockNumber(), c.selectionEnd() - b.position());
            return;
        }
    }
    const int revision = document()->revision();
    handleKey(event);
    if (!isReadOnly())
        completionTyped(event, document()->revision() != revision);
}

void CodeEditor::handleKey(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Control)
        updateLink(true);
    if (isReadOnly()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    if (autoPair(event))
        return;

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
            insertNewlineWithIndent();
            return;
        }
        break;
    case Qt::Key_Tab:
        if (mods == Qt::NoModifier) {
            if (textCursor().hasSelection() &&
                textCursor().document()->findBlock(textCursor().selectionStart()) !=
                    textCursor().document()->findBlock(textCursor().selectionEnd())) {
                indentSelection(false);
            } else {
                QTextCursor c = textCursor();
                auto &s = SettingsManager::instance();
                if (s.useSpaces()) {
                    // Advance to the next tab stop rather than always inserting N spaces.
                    const int col = document()->findBlock(c.selectionStart()).position() >= 0
                        ? c.selectionStart() - c.document()->findBlock(c.selectionStart()).position() : 0;
                    const int n = s.tabSize() - (col % s.tabSize());
                    c.insertText(QString(n, QLatin1Char(' ')));
                } else {
                    c.insertText(QStringLiteral("\t"));
                }
                setTextCursor(c);
            }
            return;
        }
        break;
    case Qt::Key_Backtab:
        indentSelection(true);
        return;
    case Qt::Key_Backspace:
        if (mods == Qt::NoModifier) {
            if (eraseEmptyPair())
                return;
            handleBackspaceInIndent(event);
            return;
        }
        break;
    case Qt::Key_Slash:
        // `<` then `/` on a whitespace-only line starts a closing tag: it lines up with its opener, one level out.
        if (isMarkupLanguage(m_language) && !textCursor().hasSelection()) {
            QTextCursor c = textCursor();
            const QString line = c.block().text();
            const QString unit = indentUnit();
            if (line.trimmed() == QLatin1String("<") && c.positionInBlock() == line.size() && line.size() > unit.size() + 1 &&
                line.startsWith(unit)) {
                c.movePosition(QTextCursor::StartOfBlock);
                c.setPosition(c.position() + unit.size(), QTextCursor::KeepAnchor);
                c.removeSelectedText();
                c.movePosition(QTextCursor::EndOfBlock);
                setTextCursor(c);
            }
        }
        break;
    case Qt::Key_ParenRight:
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
            // Typing a closing bracket on a whitespace-only line dedents one level.
            QTextCursor c = textCursor();
            const QString line = c.block().text();
            if (!c.hasSelection() && !line.isEmpty() && line.trimmed().isEmpty() && c.positionInBlock() == line.size()) {
                const QString unit = indentUnit();
                if (line.endsWith(unit)) {
                    c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, unit.size());
                    c.removeSelectedText();
                    setTextCursor(c);
                }
            }
        }
        break;
    default:
        break;
    }
    const bool wasOverwrite = overwriteMode();
    QPlainTextEdit::keyPressEvent(event);
    if (wasOverwrite != overwriteMode())
        emit overwriteModeToggled();
}

void CodeEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseDoubleClickEvent(event);
    m_tripleClickArmed = true;
    m_tripleClickTimer.start();
}

// Ctrl held over an identifier that Go to Definition can follow: underline it and show a hand cursor.
void CodeEditor::updateLink(bool ctrlDown)
{
    QPair<int, int> link{-1, -1};
    if (ctrlDown && viewport()->underMouse() && m_canGoToDefinition && m_canGoToDefinition()) {
        const QTextCursor at = cursorForPosition(viewport()->mapFromGlobal(QCursor::pos()));
        const QString text = at.block().text();
        const int col = at.positionInBlock();
        if (col < text.size() && isIdentifierChar(text.at(col))) {
            int a = col, b = col;
            while (a > 0 && isIdentifierChar(text.at(a - 1)))
                --a;
            while (b < text.size() && isIdentifierChar(text.at(b)))
                ++b;
            link = {at.block().position() + a, at.block().position() + b};
        }
    }
    viewport()->setCursor(link.first >= 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
    if (link == m_link)
        return;
    m_link = link;
    refreshSelections();
}

void CodeEditor::mouseMoveEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseMoveEvent(event);
    updateLink(event->modifiers() == Qt::ControlModifier);
    for (const auto &pill : std::as_const(m_foldPills))
        if (pill.first.contains(event->pos())) {
            viewport()->setCursor(Qt::PointingHandCursor);
            break;
        }
}

void CodeEditor::keyReleaseEvent(QKeyEvent *event)
{
    QPlainTextEdit::keyReleaseEvent(event);
    if (event->key() == Qt::Key_Control)
        updateLink(false);
}

void CodeEditor::focusOutEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusOutEvent(event);
    updateLink(false);
    hideCompletion();
}

void CodeEditor::leaveEvent(QEvent *event)
{
    QPlainTextEdit::leaveEvent(event);
    updateLink(false);
}

void CodeEditor::mousePressEvent(QMouseEvent *event)
{
    hideCompletion();
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::ControlModifier) {
        const QTextCursor at = cursorForPosition(event->pos());
        const QString text = at.block().text();
        const int col = at.positionInBlock();
        if (col < text.size() && isIdentifierChar(text.at(col))) {
            emit definitionRequested(at.blockNumber(), col);
            updateLink(false);
            return;
        }
    }
    if (event->button() == Qt::LeftButton)
        for (const auto &row : std::as_const(m_stickyRows))
            if (row.first.contains(event->pos())) {
                QTextCursor c(document()->findBlockByNumber(row.second));
                setTextCursor(c);
                centerCursor();
                return;
            }
    if (event->button() == Qt::LeftButton)
        for (const auto &pill : std::as_const(m_foldPills))
            if (pill.first.contains(event->pos())) {
                toggleFoldAt(pill.second);
                return;
            }
    if (m_tripleClickArmed && event->button() == Qt::LeftButton &&
        m_tripleClickTimer.elapsed() < QApplication::doubleClickInterval()) {
        m_tripleClickArmed = false;
        QTextCursor c = cursorForPosition(event->pos());
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        if (!c.hasSelection()) { // last line has no following block
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        }
        setTextCursor(c);
        return;
    }
    m_tripleClickArmed = false;
    QPlainTextEdit::mousePressEvent(event);
}

// --- Drag & drop / paste of files -------------------------------------------

bool CodeEditor::canInsertFromMimeData(const QMimeData *source) const
{
    return source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
}

void CodeEditor::insertFromMimeData(const QMimeData *source)
{
    if (source->hasUrls()) {
        QStringList paths;
        for (const QUrl &u : source->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        if (!paths.isEmpty()) {
            emit filesDropped(paths);
            return;
        }
    }
    QPlainTextEdit::insertFromMimeData(source);
}

// --- Git change markers --------------------------------------------------------

void CodeEditor::setDiffBase(const QStringList &lines)
{
    m_base = lines;
    m_hasBase = true;
    recomputeDiff();
}

void CodeEditor::clearDiffBase()
{
    m_hasBase = false;
    m_base.clear();
    m_diffTimer->stop();
    recomputeDiff();
}

void CodeEditor::recomputeDiff()
{
    m_hunks.clear();
    m_hunkAtLine.clear();
    m_deletedAt.clear();
    if (m_hasBase) {
        QStringList cur;
        cur.reserve(blockCount());
        for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
            cur << b.text();
        m_hunks = GitDiff::compute(m_base, cur);
        for (int i = 0; i < m_hunks.size(); ++i) {
            const GitDiff::Hunk &h = m_hunks.at(i);
            if (h.isDeleted())
                m_deletedAt.insert(h.newStart, i);
            else
                for (int l = h.newStart; l < h.newStart + h.newCount; ++l)
                    m_hunkAtLine.insert(l, i);
        }
    }
    m_lineArea->update();
}

void CodeEditor::gotoChange(bool next)
{
    if (m_hunks.isEmpty())
        return;
    const int line = textCursor().blockNumber();
    int target = -1;
    if (next) {
        for (int i = 0; i < m_hunks.size(); ++i)
            if (m_hunks.at(i).newStart > line) {
                target = i;
                break;
            }
        if (target < 0)
            target = 0;
    } else {
        for (int i = m_hunks.size() - 1; i >= 0; --i)
            if (m_hunks.at(i).newStart < line) {
                target = i;
                break;
            }
        if (target < 0)
            target = m_hunks.size() - 1;
    }
    const int l = qMin(m_hunks.at(target).newStart, blockCount() - 1);
    QTextCursor c(document()->findBlockByNumber(l));
    setTextCursor(c);
    centerCursor();
}

void CodeEditor::setBlame(const QVector<GitBlameLine> &lines)
{
    m_blame = lines;
    m_blameBlocks = blockCount();
    updateLineNumberAreaWidth();
    m_lineArea->update();
    viewport()->update();
}

void CodeEditor::clearBlame()
{
    if (m_blame.isEmpty())
        return;
    m_blame.clear();
    updateLineNumberAreaWidth();
    m_lineArea->update();
    viewport()->update();
}

void CodeEditor::setBlameGutter(bool on)
{
    m_blameGutter = on;
    updateLineNumberAreaWidth();
    m_lineArea->update();
}

void CodeEditor::setBlameInline(bool on)
{
    m_blameInline = on;
    viewport()->update();
}

// Keeps bookmarks and the blame list aligned with the text while it is edited: new lines and touched
// lines count as uncommitted.
void CodeEditor::trackLineEdit(int position, int, int)
{
    const int now = document()->blockCount();
    const int delta = now - m_blameBlocks;
    m_blameBlocks = now;
    if (delta == 0 && m_blame.isEmpty())
        return;
    const int edited = document()->findBlock(position).blockNumber();
    if (delta != 0 && !m_bookmarks.isEmpty()) {
        QList<int> moved;
        for (int l : std::as_const(m_bookmarks)) {
            if (l <= edited)
                moved.append(l);
            else if (delta > 0)
                moved.append(l + delta);
            else if (l > edited - delta)
                moved.append(l + delta); // lines inside the removed range disappear with their bookmark
        }
        if (moved != m_bookmarks) {
            m_bookmarks = moved;
            m_lineArea->update();
            emit bookmarksChanged();
        }
    }
    if (m_blame.isEmpty())
        return;
    const int line = qBound(0, edited, qMax(0, m_blame.size() - 1));
    if (delta > 0)
        m_blame.insert(qMin(line + 1, m_blame.size()), delta, GitBlameLine{});
    else if (delta < 0)
        m_blame.remove(qMin(line + 1, m_blame.size()), qMin(-delta, m_blame.size() - qMin(line + 1, m_blame.size())));
    for (int i = line; i <= line + qMax(0, delta) && i < m_blame.size(); ++i)
        m_blame[i] = GitBlameLine{};
    m_lineArea->update();
    viewport()->update();
}

void CodeEditor::setBookmarks(const QList<int> &lines)
{
    m_bookmarks = lines;
    std::sort(m_bookmarks.begin(), m_bookmarks.end());
    m_bookmarks.erase(std::unique(m_bookmarks.begin(), m_bookmarks.end()), m_bookmarks.end());
    m_lineArea->update();
}

void CodeEditor::toggleBookmark(int line)
{
    if (line < 0)
        line = textCursor().blockNumber();
    if (m_bookmarks.contains(line))
        m_bookmarks.removeOne(line);
    else {
        m_bookmarks.append(line);
        std::sort(m_bookmarks.begin(), m_bookmarks.end());
    }
    m_lineArea->update();
    emit bookmarksChanged();
}

void CodeEditor::paintBlameAnnotation()
{
    if (!m_blameInline || m_blame.isEmpty())
        return;
    const int line = textCursor().blockNumber();
    if (line >= m_blame.size() || !textCursor().block().isVisible())
        return;
    const GitBlameLine &bl = m_blame.at(line);
    const QString text = bl.committed() ? bl.author + QStringLiteral(", ") + relativeTime(bl.time) + QStringLiteral("  \u2022  ") + bl.summary
                                        : tr("You, uncommitted changes");
    QTextCursor end(textCursor().block());
    end.movePosition(QTextCursor::EndOfBlock);
    const QRect r = cursorRect(end);
    const int x = r.right() + fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4;
    const int avail = viewport()->width() - x - 12;
    if (avail < 80)
        return;
    QPainter p(viewport());
    QFont f = font();
    f.setItalic(true);
    p.setFont(f);
    QColor c = m_gutterFg;
    c.setAlpha(200);
    p.setPen(c);
    p.drawText(QRect(x, r.top(), avail, r.height()), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(text, Qt::ElideRight, avail));
}

bool CodeEditor::foldIconAt(const QPoint &pos) const
{
    if (pos.x() < m_lineArea->width() - kFoldWidth)
        return false;
    const QTextBlock b = cursorForPosition(QPoint(0, pos.y())).block();
    return b.isValid() && (isFolded(b) || isFoldable(b));
}

void CodeEditor::gutterClicked(const QPoint &pos)
{
    if (pos.x() >= m_lineArea->width() - kFoldWidth) {
        const QTextBlock b = cursorForPosition(QPoint(0, pos.y())).block();
        if (b.isValid() && (isFolded(b) || isFoldable(b)))
            toggleFoldAt(b.blockNumber());
        return;
    }
    if (blameWidth() > 0 && pos.x() < blameWidth() && (pos.x() > 8 || !m_hasBase)) {
        const int bl = cursorForPosition(QPoint(0, pos.y())).blockNumber();
        if (bl < m_blame.size() && m_blame.at(bl).committed())
            emit blameCommitRequested(m_blame.at(bl).hash);
        return;
    }
    if (!m_hasBase || pos.x() > 8)
        return;
    const int line = cursorForPosition(QPoint(0, pos.y())).blockNumber();
    int idx = m_hunkAtLine.value(line, -1);
    if (idx < 0)
        idx = m_deletedAt.value(line, -1);
    if (idx < 0 && line == blockCount() - 1)
        idx = m_deletedAt.value(line + 1, -1);
    if (idx >= 0)
        showHunkPopup(idx, m_lineArea->mapToGlobal(QPoint(m_lineArea->width(), pos.y())));
}

void CodeEditor::showHunkPopup(int hunkIndex, const QPoint &globalPos)
{
    const GitDiff::Hunk h = m_hunks.at(hunkIndex);
    auto *popup = new QFrame(this, Qt::Popup);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setFrameShape(QFrame::StyledPanel);
    auto *lay = new QVBoxLayout(popup);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    QString what;
    if (h.isAdded())
        what = tr("Added %n line(s)", nullptr, h.newCount);
    else if (h.isDeleted())
        what = tr("Removed %n line(s)", nullptr, h.oldCount);
    else
        what = tr("Changed %n line(s)", nullptr, h.newCount);
    auto *title = new QLabel(what, popup);
    title->setObjectName(QStringLiteral("emptyText"));
    lay->addWidget(title);

    if (h.oldCount > 0) {
        auto *view = new QPlainTextEdit(popup);
        view->setReadOnly(true);
        view->setFont(font());
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        view->setPlainText(m_base.mid(h.oldStart, h.oldCount).join(QLatin1Char('\n')));
        QPalette pal = view->palette();
        pal.setColor(QPalette::Base, m_diffDelBg);
        pal.setColor(QPalette::Text, palette().color(QPalette::Text));
        view->setPalette(pal);
        const int lines = qMin(h.oldCount, 12);
        view->setFixedHeight(fontMetrics().lineSpacing() * lines + 14);
        view->setMinimumWidth(qMin(width() - lineNumberAreaWidth() - 40, 520));
        lay->addWidget(view);
    }

    auto *row = new QHBoxLayout;
    auto *revert = new QPushButton(tr("Revert Change"), popup);
    connect(revert, &QPushButton::clicked, this, [this, popup, hunkIndex] {
        popup->close();
        revertHunk(hunkIndex);
    });
    row->addWidget(revert);
    row->addStretch(1);
    lay->addLayout(row);
    popup->move(globalPos);
    popup->show();
}

// Puts the committed text of one hunk back into the buffer (undoable as a single step).
void CodeEditor::revertHunk(int hunkIndex)
{
    if (hunkIndex < 0 || hunkIndex >= m_hunks.size() || isReadOnly())
        return;
    const GitDiff::Hunk h = m_hunks.at(hunkIndex);
    const QStringList oldLines = m_base.mid(h.oldStart, h.oldCount);
    QTextDocument *doc = document();
    const int blocks = doc->blockCount();
    const bool endsAtBlock = h.newStart + h.newCount < blocks;

    QTextCursor c(doc);
    c.beginEditBlock();
    if (h.newCount == 0) {
        // Pure deletion: re-insert the old lines before `newStart` (or after the last line).
        if (h.newStart < blocks) {
            c.setPosition(doc->findBlockByNumber(h.newStart).position());
            c.insertText(oldLines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        } else {
            c.movePosition(QTextCursor::End);
            c.insertText(QLatin1Char('\n') + oldLines.join(QLatin1Char('\n')));
        }
    } else {
        int start = doc->findBlockByNumber(h.newStart).position();
        const int end = endsAtBlock ? doc->findBlockByNumber(h.newStart + h.newCount).position()
                                    : doc->findBlockByNumber(blocks - 1).position() + doc->findBlockByNumber(blocks - 1).length() - 1;
        QString replacement = oldLines.join(QLatin1Char('\n'));
        if (endsAtBlock)
            replacement += oldLines.isEmpty() ? QString() : QStringLiteral("\n");
        else if (oldLines.isEmpty() && h.newStart > 0)
            start = doc->findBlockByNumber(h.newStart - 1).position() + doc->findBlockByNumber(h.newStart - 1).length() - 1;
        c.setPosition(start);
        c.setPosition(end, QTextCursor::KeepAnchor);
        c.insertText(replacement);
    }
    c.endEditBlock();
    recomputeDiff();
}

// --- Completion -----------------------------------------------------------------------------------

bool CodeEditor::completionVisible() const { return m_completion && m_completion->isVisible(); }

bool CodeEditor::inStringOrComment(int position) const
{
    if (position <= 0)
        return false;
    const QTextBlock block = document()->findBlock(position - 1);
    const QVector<bool> mask = protectedMask(block);
    const int i = position - 1 - block.position();
    return i >= 0 && i < mask.size() && mask[i];
}

QString CodeEditor::completionPrefix() const
{
    const int cur = textCursor().position();
    if (m_completionAnchor < 0 || cur < m_completionAnchor)
        return {};
    QTextCursor c(document());
    c.setPosition(m_completionAnchor);
    c.setPosition(cur, QTextCursor::KeepAnchor);
    return c.selectedText();
}

void CodeEditor::hideCompletion()
{
    if (m_completionTimer)
        m_completionTimer->stop();
    if (m_completion)
        m_completion->hide();
    ++m_completionToken; // an answer still on its way is stale now
    m_completionAnchor = -1;
}

bool CodeEditor::completionKey(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods != Qt::NoModifier)
            return false;
        m_completion->moveSelection(event->key() == Qt::Key_Up ? -1 : 1);
        return true;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        m_completion->moveSelection(event->key() == Qt::Key_PageUp ? -8 : 8);
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods != Qt::NoModifier)
            return false;
        acceptCompletion();
        return true;
    case Qt::Key_Escape:
        hideCompletion();
        return true;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
        hideCompletion();
        return false;
    default:
        return false;
    }
}

void CodeEditor::completionTyped(QKeyEvent *event, bool edited)
{
    const bool served = m_canGoToDefinition && m_canGoToDefinition();
    const int cur = textCursor().position();
    if (completionVisible() && (cur < m_completionAnchor || !edited)) { // moved away from the word
        if (!edited && cur >= m_completionAnchor && completionPrefix().size() == cur - m_completionAnchor)
            return;
        hideCompletion();
        return;
    }
    if (!edited)
        return;

    const bool erase = event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete;
    const QString text = event->text();
    if (erase) {
        if (!completionVisible())
            return;
        if (m_completion->setPrefix(completionPrefix()) == 0)
            hideCompletion();
        return;
    }
    if (text.size() != 1 || text[0].isSpace() || text[0].category() == QChar::Other_Control) {
        hideCompletion();
        return;
    }
    const QChar ch = text[0];
    if (isIdentChar(ch)) {
        if (completionVisible()) {
            const QString prefix = completionPrefix();
            if (prefix.isEmpty() || m_completion->setPrefix(prefix) == 0) {
                hideCompletion();
            } else if (m_completionIncomplete) { // the server cut its list short: ask again for the longer prefix
                scheduleCompletion(3, {});
            }
            return;
        }
        if (!served || (completesInStrings(m_language) ? inCommentAt(document(), cur) : inStringOrComment(cur)))
            return;
        QTextCursor c(document());
        c.setPosition(cur);
        int start = cur;
        while (start > 0 && isIdentChar(document()->characterAt(start - 1)))
            --start;
        if (document()->characterAt(start).isDigit()) // a number literal, not a name
            return;
        scheduleCompletion(1, {});
        return;
    }
    hideCompletion();
    if (!served || (completesInStrings(m_language) ? inCommentAt(document(), cur - 1) : inStringOrComment(cur - 1)))
        return;
    const QChar prev = cur >= 2 ? document()->characterAt(cur - 2) : QChar();
    // Web servers: tags and closing tags (< and /), import paths and attribute values (quotes, /, @), CSS (: - @).
    if (completesInStrings(m_language)) {
        const bool css = m_language == QLatin1String("CSS");
        const bool html = m_language == QLatin1String("HTML");
        const QString triggers = css ? QStringLiteral(":-@") : html ? QStringLiteral("<\"'/=") : QStringLiteral("\"'`/@<");
        if (ch == QLatin1Char('.') || triggers.contains(ch)) {
            scheduleCompletion(2, QString(ch));
            return;
        }
    }
    if (ch == QLatin1Char('.') || (ch == QLatin1Char('>') && prev == QLatin1Char('-')) || (ch == QLatin1Char(':') && prev == QLatin1Char(':')))
        scheduleCompletion(2, ch == QLatin1Char('>') ? QStringLiteral(">") : QString(ch));
}

void CodeEditor::scheduleCompletion(int kind, const QString &triggerChar)
{
    if (!m_completionTimer) {
        m_completionTimer = new QTimer(this);
        m_completionTimer->setSingleShot(true);
        connect(m_completionTimer, &QTimer::timeout, this, [this] { requestCompletion(m_pendingKind, m_pendingTrigger); });
    }
    m_pendingKind = kind;
    m_pendingTrigger = triggerChar;
    m_completionTimer->start(60);
}

void CodeEditor::triggerCompletion()
{
    requestCompletion(1, {});
}

void CodeEditor::requestCompletion(int kind, const QString &triggerChar)
{
    const int cur = textCursor().position();
    int start = cur;
    while (start > 0 && isIdentChar(document()->characterAt(start - 1)))
        --start;
    m_completionAnchor = start;
    const int token = ++m_completionToken;
    const bool served = m_canGoToDefinition && m_canGoToDefinition();
    if (served) {
        const QTextCursor c = textCursor();
        emit completionRequested(c.blockNumber(), c.positionInBlock(), kind, triggerChar, token);
        return;
    }
    // No server: offer the words that already appear in the document.
    QSet<QString> seen;
    QVector<LspCompletionItem> items;
    static const QRegularExpression word(QStringLiteral("[A-Za-z_][A-Za-z0-9_]{2,}"));
    const QString all = toPlainText();
    for (auto it = word.globalMatch(all); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() == start || seen.contains(m.captured()))
            continue;
        seen.insert(m.captured());
        LspCompletionItem item;
        item.label = m.captured();
        item.kind = 1;
        items.append(item);
        if (items.size() >= 5000)
            break;
    }
    showCompletions(items, false, token);
}

void CodeEditor::showCompletions(const QVector<LspCompletionItem> &items, bool incomplete, int token)
{
    if (token != m_completionToken || m_completionAnchor < 0 || !hasFocus())
        return;
    const int cur = textCursor().position();
    const QString prefix = completionPrefix();
    bool wordOnly = cur >= m_completionAnchor;
    for (const QChar c : prefix)
        wordOnly = wordOnly && isIdentChar(c);
    if (!wordOnly || items.isEmpty()) {
        hideCompletion();
        return;
    }
    if (!m_completion) {
        m_completion = new CompletionPopup(this);
        m_completion->setFont(font());
        connect(m_completion, &CompletionPopup::accepted, this, &CodeEditor::acceptCompletion);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &CodeEditor::hideCompletion);
    }
    m_completionIncomplete = incomplete;
    m_completion->setItems(items);
    if (m_completion->setPrefix(prefix) == 0) {
        hideCompletion();
        return;
    }
    QTextCursor anchor(document());
    anchor.setPosition(m_completionAnchor);
    const QRect r = cursorRect(anchor);
    m_completion->popup(viewport()->mapToGlobal(r.bottomLeft() + QPoint(0, 2)), viewport()->mapToGlobal(r.topLeft() - QPoint(0, 2)));
}

namespace {
// "foo(${1:a}, $2)$0" -> "foo(a, )"
QString stripSnippet(QString text)
{
    static const QRegularExpression placeholder(QStringLiteral("\\$\\{\\d+:([^}]*)\\}"));
    static const QRegularExpression tabstop(QStringLiteral("\\$(\\{\\d+\\}|\\d+)"));
    text.replace(placeholder, QStringLiteral("\\1"));
    text.remove(tabstop);
    text.replace(QStringLiteral("\\$"), QStringLiteral("$"));
    return text;
}
}

void CodeEditor::acceptCompletion()
{
    if (!m_completion)
        return;
    const LspCompletionItem *picked = m_completion->current();
    if (!picked) {
        hideCompletion();
        return;
    }
    const LspCompletionItem item = *picked;
    const int cur = textCursor().position();
    const int anchor = m_completionAnchor;
    hideCompletion();

    if (!item.command.isEmpty() && m_commandRunner) {
        const QTextCursor c = textCursor();
        const int revision = document()->revision();
        QPointer<CodeEditor> guard(this);
        const bool taken = m_commandRunner(item, c.blockNumber(), c.positionInBlock(), [guard, item, anchor, cur, revision](bool done) {
            if (!done && guard && guard->document()->revision() == revision)
                guard->insertCompletion(item, anchor, cur);
        });
        if (taken)
            return;
    }
    insertCompletion(item, anchor, cur);
}

bool CodeEditor::applyTextEdits(const QVector<LspTextEdit> &list)
{
    if (isReadOnly())
        return false;
    QTextDocument *doc = document();
    auto position = [doc](int line, int column) {
        const QTextBlock b = doc->findBlockByNumber(line);
        if (!b.isValid())
            return doc->characterCount() - 1;
        return b.position() + qMin(column, b.length() - 1);
    };
    struct Edit {
        int start, end;
        QString text;
    };
    QVector<Edit> edits;
    for (const LspTextEdit &e : list)
        edits.append({position(e.startLine, e.startColumn), position(e.endLine, e.endColumn), e.text});
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start > b.start; });
    QTextCursor c(doc);
    c.beginEditBlock();
    for (const Edit &e : std::as_const(edits)) {
        c.setPosition(e.start);
        c.setPosition(e.end, QTextCursor::KeepAnchor);
        c.insertText(e.text);
    }
    c.endEditBlock();
    return true;
}

void CodeEditor::insertCompletion(const LspCompletionItem &item, int anchor, int cur)
{
    QTextDocument *doc = document();
    auto position = [doc](int line, int column) {
        const QTextBlock b = doc->findBlockByNumber(line);
        if (!b.isValid())
            return doc->characterCount() - 1;
        return b.position() + qMin(column, b.length() - 1);
    };
    struct Edit {
        int start, end;
        QString text;
    };
    QString text = item.hasEdit ? item.edit.text : (item.insertText.isEmpty() ? item.label.trimmed() : item.insertText);
    if (item.snippet)
        text = stripSnippet(text);
    int start = anchor, end = cur;
    if (item.hasEdit) {
        start = position(item.edit.startLine, item.edit.startColumn);
        end = qMax(position(item.edit.endLine, item.edit.endColumn), cur); // keep what was typed since the request
    }
    QVector<Edit> edits{{start, end, text}};
    for (const LspTextEdit &e : item.additionalEdits)
        edits.append({position(e.startLine, e.startColumn), position(e.endLine, e.endColumn), e.text});
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start > b.start; });

    int shift = 0; // how much the extra edits above the insertion moved it
    for (const Edit &e : std::as_const(edits))
        if (e.end <= start && !(e.start == start && e.end == end))
            shift += e.text.size() - (e.end - e.start);

    QTextCursor c(doc);
    c.beginEditBlock();
    for (const Edit &e : std::as_const(edits)) {
        c.setPosition(e.start);
        c.setPosition(e.end, QTextCursor::KeepAnchor);
        c.insertText(e.text);
    }
    c.endEditBlock();
    c.setPosition(start + shift + text.size());
    setTextCursor(c);
    ensureCursorVisible();
}
