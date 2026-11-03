#include "CodeEditor.h"

#include "Breadcrumbs.h"
#include "CodeEditorText.h"
#include "CompletionPopup.h"
#include "HoverPopup.h"
#include "HoverRender.h"
#include "MiniMap.h"
#include "SignaturePopup.h"
#include "SyntaxHighlighter.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFrame>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

using namespace CodeEditorText;

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
        setCursor(m_editor->foldIconAt(event->pos()) || m_editor->runMarkerAt(event->pos()) || m_editor->breakpointAreaAt(event->pos())
                      ? Qt::PointingHandCursor
                      : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            if (m_editor->runMarkerAt(help->pos()))
                QToolTip::showText(help->globalPos(), m_editor->runMarkerTip(), this);
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(event);
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

} // namespace

QString CodeEditorText::relativeTime(qint64 secs)
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

QVector<bool> CodeEditorText::protectedMask(const QTextBlock &block)
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

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    m_lineArea = new LineNumberArea(this);
    m_minimap = new MiniMap(this);
    m_matchTimer = new QTimer(this);
    m_matchTimer->setSingleShot(true);
    m_matchTimer->setInterval(150);
    connect(m_matchTimer, &QTimer::timeout, this, &CodeEditor::recomputeMatches);
    m_colorTimer = new QTimer(this);
    m_colorTimer->setSingleShot(true);
    m_colorTimer->setInterval(450);
    connect(m_colorTimer, &QTimer::timeout, this, &CodeEditor::colorsRequested);
    setupAssists();
    setupPredictions();

    setFrameShape(QFrame::NoFrame);
    setStyleSheet(QStringLiteral("QPlainTextEdit { border: none; border-radius: 0; }")); // no focus ring inside the island
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    viewport()->setMouseTracking(true);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::refreshSelections);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!m_blame.isEmpty() || !m_predictions.isEmpty())
            viewport()->update();
    });
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (m_gapUpdating)
            return;
        updateGuideScope();
        if (m_foldedCount > 0)
            m_foldTimer->start();
        if (!m_term.isEmpty())
            m_matchTimer->start();
        if (m_hasBase)
            m_diffTimer->start();
        m_colorTimer->start();
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
    connect(&SettingsManager::instance(), &SettingsManager::projectSettingsChanged, this, &CodeEditor::applySettings);
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
    m_matchBg = t.findMatchBg;
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
    disconnect(m_predictConn);
    m_predictConn = connect(doc, &QTextDocument::contentsChange, this, &CodeEditor::predictionChange);
    resetPredictionText();
    applySettings();
    updateLineNumberAreaWidth();
    updateGuideScope();
    m_minimap->invalidate();
}

void CodeEditor::setLanguage(const QString &name)
{
    const bool wrapChanged = (name == QLatin1String("Markdown")) != (m_language == QLatin1String("Markdown"));
    m_language = name;
    if (wrapChanged)
        applySettings();
    viewport()->update();
}

void CodeEditor::applySettings()
{
    auto &s = SettingsManager::instance();
    const QFont f = s.editorFont();
    setFont(f);
    // setFont() does not reach the document when the widget font is unchanged. Qt lays the text out again even when the
    // font is the same, which on a file with a megabyte-long line costs seconds: only set a different one.
    if (document()->defaultFont() != f)
        document()->setDefaultFont(f);
    setTabStopDistance(QFontMetricsF(f).horizontalAdvance(QLatin1Char(' ')) * tabWidth());
    // Prose never scrolls sideways: Markdown always wraps, whatever the global setting says.
    setLineWrapMode(s.wordWrap() || m_language == QLatin1String("Markdown") ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_indentGuides = s.indentGuides();
    m_stickyScroll = s.stickyScroll();
    m_blameInline = s.blameInline();
    m_blameGutter = s.blameGutter();
    m_showMinimap = s.showMinimap() && !m_largeFile;
    m_predictEnabled = s.editPredictions() && !m_largeFile;
    if (!m_predictEnabled)
        clearPredictions();
    requestInlayHintsSoon(); // the font (and so the room hints need) may have changed
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
    return blameWidth() + 14 + (m_bulbEnabled || !m_runLines.isEmpty() ? kBulbWidth : 0) + kFoldWidth + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
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

void CodeEditor::scrollBlockToTop(int blockNumber)
{
    int lines = 0;
    for (QTextBlock b = document()->begin(); b.isValid() && b.blockNumber() < blockNumber; b = b.next())
        lines += b.isVisible() ? qMax(1, b.lineCount()) : 0;
    verticalScrollBar()->setValue(lines);
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
            const bool breakpoint = !m_breakpoints.isEmpty() && m_breakpoints.contains(number);
            if (breakpoint || number == m_execLine) {
                // A breakpoint is a red disc behind the number; the debugger's line an arrow in the theme's warning colour.
                const Theme theme = Theme::byName(SettingsManager::instance().theme());
                const int h = fontMetrics().height();
                const int right = m_lineArea->width() - 4 - kFoldWidth;
                const int w = qMax(h, fontMetrics().horizontalAdvance(QString::number(number + 1)) + 8);
                const QRectF r(right - w, top + 1, w, h - 2);
                const QPen numberPen = painter.pen();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                if (breakpoint) {
                    painter.setBrush(theme.danger);
                    painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
                }
                if (number == m_execLine) {
                    QPainterPath arrow;
                    const qreal ax = r.left() - (breakpoint ? 2 : 0), ay = r.center().y(), hh = r.height() / 2;
                    arrow.moveTo(ax - 8, ay - hh + 2);
                    arrow.lineTo(ax - 2, ay);
                    arrow.lineTo(ax - 8, ay + hh - 2);
                    arrow.closeSubpath();
                    painter.setBrush(theme.warning);
                    painter.drawPath(arrow);
                }
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.setPen(breakpoint ? QPen(theme.editorBg) : numberPen);
            }
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
            if (m_bulbEnabled && number == m_bulbLine)
                paintLightBulb(painter, top, fontMetrics().height());
            else if (!m_runLines.isEmpty() && m_runLines.contains(number))
                paintRunMarker(painter, top, fontMetrics().height());
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

CodeEditor::~CodeEditor()
{
    // The document (and its highlighter) may outlive the editor: the highlighter must not call back into it.
    if (auto *hl = document() ? document()->findChild<SyntaxHighlighter *>() : nullptr)
        hl->setGapProvider({});
}

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
    if (!m_breakpoints.isEmpty() || m_execLine >= 0) {
        const Theme theme = Theme::byName(SettingsManager::instance().theme());
        QColor bp = theme.danger, exec = theme.warning;
        bp.setAlpha(theme.dark ? 34 : 28);
        exec.setAlpha(theme.dark ? 70 : 90);
        auto lineSel = [&](int number, const QColor &bg) {
            const QTextBlock b = document()->findBlockByNumber(number);
            if (!b.isValid())
                return;
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(bg);
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sel.cursor = QTextCursor(b);
            extra.append(sel);
        };
        for (int l : std::as_const(m_breakpoints))
            if (l != m_execLine)
                lineSel(l, bp);
        if (m_execLine >= 0)
            lineSel(m_execLine, exec);
    }

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
    if (!m_highlights.isEmpty()) {
        const Theme theme = Theme::byName(SettingsManager::instance().theme());
        QColor read = theme.accent, write = theme.accent;
        read.setAlpha(theme.dark ? 46 : 40);
        write.setAlpha(theme.dark ? 96 : 80);
        for (const Highlight &h : std::as_const(m_highlights)) {
            if (h.from.isNull() || h.to.isNull() || h.from.position() >= h.to.position())
                continue;
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(h.kind == 3 ? write : read);
            sel.cursor = QTextCursor(document());
            sel.cursor.setPosition(h.from.position());
            sel.cursor.setPosition(h.to.position(), QTextCursor::KeepAnchor);
            extra.append(sel);
        }
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
    appendPredictionSelections(extra);
    appendCursorSelections(extra);
    extra += m_inlineMarks;
    setExtraSelections(extra);
    m_lineArea->update();
}

void CodeEditor::setInlineMarks(const QList<QTextEdit::ExtraSelection> &marks)
{
    if (marks.isEmpty() && m_inlineMarks.isEmpty())
        return;
    m_inlineMarks = marks;
    refreshSelections();
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

bool CodeEditor::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QTextCursor at = cursorForPosition(help->pos());
        const int pos = at.position();
        if (m_tooltipProvider) {
            const QString tip = m_tooltipProvider(at.blockNumber(), at.positionInBlock());
            if (!tip.isEmpty()) {
                QToolTip::showText(help->globalPos(), tip, viewport());
                return true;
            }
        }
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
        m_hoverFromCaret = false;
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
        // A hyphen inside a name counts (Tailwind / CSS class names: bg-red-500, margin-top).
        const bool dashInWord = col < text.size() && text.at(col) == QLatin1Char('-') &&
                                ((col > 0 && isIdentifierChar(text.at(col - 1))) || (col + 1 < text.size() && isIdentifierChar(text.at(col + 1))));
        const bool onWord = col < text.size() && (isIdentifierChar(text.at(col)) || dashInWord) && help->pos().y() < lineRect.bottom();
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

namespace {

// Server hover text made readable: links to source files (jar:///…, file:///…, jdt://…: hundreds of characters that
// only eat the length budget and cannot be followed from a tooltip) become plain text, compiler-internal annotations
// leave the signature, and a trailing signature block (Kotlin puts it after the documentation) moves to the top.
QString tidyHoverMarkdown(const QString &markdown)
{
    static const QRegularExpression link(QStringLiteral(R"(\[([^\]\n]*)\]\(([^)\s]*)\))"));
    QString md;
    int last = 0;
    auto it = link.globalMatch(markdown);
    while (it.hasNext()) {
        const auto m = it.next();
        md += markdown.mid(last, m.capturedStart() - last);
        const QString url = m.captured(2);
        md += url.startsWith(QLatin1String("http://")) || url.startsWith(QLatin1String("https://")) ? m.captured(0) : m.captured(1);
        last = m.capturedEnd();
    }
    md += markdown.mid(last);

    static const QRegularExpression noise(QStringLiteral(R"(^@(?:StabilityInferred|SourceDebugExtension|ComposableTarget|Metadata|Stable|Immutable)\b.*\n)"),
                                          QRegularExpression::MultilineOption);
    md.remove(noise);

    static const QRegularExpression fence(QStringLiteral(R"((?:^|\n)(`{3,})[^\n]*\n.*?\n\1[ \t]*(?:\n|$))"), QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch lastFence;
    auto fi = fence.globalMatch(md);
    while (fi.hasNext())
        lastFence = fi.next();
    if (lastFence.hasMatch() && lastFence.capturedStart() > 0 && md.mid(lastFence.capturedEnd()).trimmed().isEmpty()) {
        const QString block = lastFence.captured(0).trimmed();
        md = block + QStringLiteral("\n\n") + md.left(lastFence.capturedStart()).trimmed();
    }
    return md.trimmed();
}

} // namespace

void CodeEditor::showHover(const QString &markdown)
{
    const bool fromCaret = m_hoverFromCaret;
    m_hoverFromCaret = false;
    if (m_hoverPos < 0 || markdown.isEmpty())
        return;
    if (fromCaret) {
        if (!hasFocus())
            return;
    } else {
        if (!viewport()->underMouse())
            return;
        if (cursorForPosition(viewport()->mapFromGlobal(QCursor::pos())).position() != m_hoverPos)
            return; // the mouse moved on
    }
    constexpr int kMaxChars = 1800;
    QString md = tidyHoverMarkdown(markdown);
    if (md.size() > kMaxChars) {
        // Cut at a line end, never inside a line (a half link or half sentence looks like garbage). An unclosed code
        // fence is closed again so the rest of the popup does not turn into code.
        const int cut = md.lastIndexOf(QLatin1Char('\n'), kMaxChars);
        md = md.left(cut > kMaxChars / 2 ? cut : kMaxChars);
        if (md.count(QStringLiteral("```")) % 2)
            md += QStringLiteral("\n```");
        md += QStringLiteral("\n\n…");
    }
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    const QTextBlock hoverBlock = document()->findBlock(m_hoverPos);
    const int line = hoverBlock.blockNumber(), column = m_hoverPos - hoverBlock.position();
    QString html = HoverRender::markdownHtml(md, theme, font(), m_language.toLower());
    // Footer: what can be done with the symbol under the mouse; the popup stays open, so the links can be clicked.
    QString links = QStringLiteral("<a href=\"qode:definition\">%1</a> &nbsp;·&nbsp; <a href=\"qode:references\">%2</a>")
                        .arg(tr("Go to Definition"), tr("Find References"));
    if (!m_hoverDiagnostics.isEmpty())
        links += QStringLiteral(" &nbsp;·&nbsp; <a href=\"qode:quickfix\">%1</a>").arg(tr("Quick Fix…"));
    html += QStringLiteral("<hr><div style=\"color:%1\">%2</div>").arg(theme.textMuted.name(), links);
    if (!m_hoverDiagnostics.isEmpty())
        html = m_hoverDiagnostics + QStringLiteral("<hr>") + html;
    QToolTip::hideText();
    if (m_hoverPopup)
        m_hoverPopup->close();
    m_hoverPopup = new HoverPopup(this);
    connect(m_hoverPopup, &HoverPopup::linkActivated, this, [this, line, column](const QString &href) {
        if (href == QLatin1String("qode:definition")) {
            emit definitionRequested(line, column);
        } else if (href == QLatin1String("qode:references")) {
            emit referencesRequested(line, column);
        } else if (href == QLatin1String("qode:quickfix")) {
            const QTextBlock b = document()->findBlockByNumber(line);
            if (b.isValid()) {
                QTextCursor c(document());
                c.setPosition(b.position() + qMin(column, b.length() - 1));
                setTextCursor(c);
            }
            requestCodeActions();
        } else {
            applyHoverLink(href);
        }
    });
    m_hoverPopup->showHtml(html, m_hoverGlobal);
}

// --- Code folding -------------------------------------------------------------

void CodeEditor::setGutterHover(bool on)
{
    m_gutterHover = on;
    m_lineArea->update();
}

int CodeEditor::indentDepth(const QTextBlock &block, bool *blank) const
{
    const QString text = block.text();
    const int tab = tabWidth();
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
    paintCodeLenses();
    paintStickyScroll();
    paintBlameAnnotation();
    paintSwatches();
    paintInlayHints();
    paintPredictions();
    paintExtraCursors();
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
        for (const Breadcrumbs::Crumb &c : symbolChainAt(probe))
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
    const int tab = tabWidth();
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

void CodeEditor::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int dy = event->angleDelta().y();
        if (dy != 0) {
            auto &s = SettingsManager::instance();
            s.setUiZoom(s.uiZoom() + (dy > 0 ? 10 : -10));
        }
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    const QScopedValueRollback<bool> undoing(m_predictUndo, event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo));
    if (multiCursorShortcut(event) || (!m_extraCursors.isEmpty() && multiCursorKey(event)))
        return;
    beforeKeyEdit(event);
    const auto afterEdit = qScopeGuard([this] { afterKeyEdit(); });
    if (!isReadOnly()) {
        if (m_completion && m_completion->isVisible() && completionKey(event))
            return;
        if (signatureVisible()) {
            const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
            if (event->key() == Qt::Key_Escape) {
                hideSignatureHelp();
                return;
            }
            if (mods == Qt::NoModifier && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) && m_signature->overloads() > 1) {
                m_signature->cycle(event->key() == Qt::Key_Up ? -1 : 1); // overloads, like the popup in other editors
                return;
            }
        }
        if (event->key() == Qt::Key_Space && (event->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier) {
            triggerCompletion();
            return;
        }
        if (event->key() == Qt::Key_Space && (event->modifiers() & ~Qt::KeypadModifier) == (Qt::ControlModifier | Qt::ShiftModifier)) {
            triggerSignatureHelp();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & ~Qt::KeypadModifier) == Qt::AltModifier) {
            requestCodeActions();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier &&
            hasRunMarkers()) {
            emit runRequested(textCursor().blockNumber());
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            (event->modifiers() & ~Qt::KeypadModifier) == (Qt::ControlModifier | Qt::ShiftModifier) && hasPredictions()) {
            acceptAllPredictions();
            return;
        }
    }
    const int revision = document()->revision();
    handleKey(event);
    if (!isReadOnly()) {
        const bool edited = document()->revision() != revision;
        completionTyped(event, edited);
        signatureTyped(event, edited);
    }
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
    LinkHit hit;
    if (ctrlDown && viewport()->underMouse()) {
        const QTextCursor at = cursorForPosition(viewport()->mapFromGlobal(QCursor::pos()));
        const QString text = at.block().text();
        const int col = at.positionInBlock();
        hit = linkAt(at.position()); // a URL or a path wins over Go to Definition
        if (hit.valid()) {
            link = {hit.start, hit.end};
        } else if (m_canGoToDefinition && m_canGoToDefinition() && col < text.size() && isIdentifierChar(text.at(col))) {
            int a = col, b = col;
            while (a > 0 && isIdentifierChar(text.at(a - 1)))
                --a;
            while (b < text.size() && isIdentifierChar(text.at(b)))
                ++b;
            link = {at.block().position() + a, at.block().position() + b};
        }
    }
    viewport()->setCursor(link.first >= 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
    m_linkHit = hit;
    if (link == m_link)
        return;
    m_link = link;
    refreshSelections();
}

void CodeEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_columnDrag.active && (event->buttons() & Qt::LeftButton)) {
        if (!m_columnDrag.moved && (event->pos() - m_columnDrag.pressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        m_columnDrag.moved = true;
        columnSelect(event->pos());
        return;
    }
    QPlainTextEdit::mouseMoveEvent(event);
    updateLink(event->modifiers() == Qt::ControlModifier);
    for (const auto &pill : std::as_const(m_foldPills))
        if (pill.first.contains(event->pos())) {
            viewport()->setCursor(Qt::PointingHandCursor);
            break;
        }
    for (const Swatch &sw : std::as_const(m_swatches))
        if (sw.rect.contains(event->pos())) {
            viewport()->setCursor(Qt::PointingHandCursor);
            break;
        }
    const int lens = event->buttons() == Qt::NoButton ? codeLensAt(event->pos()) : -1;
    if (lens >= 0)
        viewport()->setCursor(Qt::PointingHandCursor);
    if (lens != m_lensHover) {
        m_lensHover = lens;
        viewport()->update();
    }
}

void CodeEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_columnDrag.active && event->button() == Qt::LeftButton) {
        m_columnDrag.active = false;
        if (!m_columnDrag.moved)
            toggleCursorAt(event->pos());
        return;
    }
    QPlainTextEdit::mouseReleaseEvent(event);
}

// With several carets, the window's Copy / Cut / Paste / Undo shortcuts and Escape come to keyPressEvent() instead.
bool CodeEditor::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride && !m_extraCursors.isEmpty()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut) || key->matches(QKeySequence::Paste) ||
            key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo) || key->matches(QKeySequence::SelectAll) ||
            key->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
    }
    return QPlainTextEdit::event(event);
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
    hideSignatureHelp();
}

void CodeEditor::leaveEvent(QEvent *event)
{
    QPlainTextEdit::leaveEvent(event);
    updateLink(false);
    if (m_lensHover >= 0) {
        m_lensHover = -1;
        viewport()->update();
    }
}

void CodeEditor::mousePressEvent(QMouseEvent *event)
{
    hideCompletion();
    hideSignatureHelp();
    if (event->button() == Qt::LeftButton && m_predictPill.contains(event->pos())) {
        acceptPrediction();
        return;
    }
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier)
        if (const int lens = codeLensAt(event->pos()); lens >= 0) {
            emit codeLensClicked(lens);
            return;
        }
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier)
        for (int i = 0; i < m_swatches.size(); ++i)
            if (m_swatches.at(i).rect.contains(event->pos())) {
                emit swatchClicked(i);
                return;
            }
    // Alt+click adds / removes a caret, Alt+drag selects a column (Alt+Shift: from the caret to the click).
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (event->button() == Qt::LeftButton && (mods == Qt::AltModifier || mods == (Qt::AltModifier | Qt::ShiftModifier))) {
        m_columnDrag = {};
        m_columnDrag.active = true;
        m_columnDrag.pressPos = event->pos();
        const bool fromCaret = mods & Qt::ShiftModifier;
        const QTextCursor anchor = fromCaret ? textCursor() : cursorForPosition(event->pos());
        m_columnDrag.anchorBlock = anchor.blockNumber();
        m_columnDrag.anchorX = (fromCaret ? cursorRect(anchor).left() : event->pos().x()) - contentOffset().x();
        if (fromCaret) {
            m_columnDrag.moved = true;
            columnSelect(event->pos());
        }
        return;
    }
    if (event->button() == Qt::LeftButton)
        clearExtraCursors();
    if (event->button() == Qt::LeftButton && mods == Qt::ControlModifier) {
        const QTextCursor at = cursorForPosition(event->pos());
        const QString text = at.block().text();
        const int col = at.positionInBlock();
        if (const LinkHit hit = linkAt(at.position()); hit.valid()) {
            openLink(hit);
            updateLink(false);
            return;
        }
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
        int start, end, index;
        QString text;
    };
    QVector<Edit> edits;
    for (const LspTextEdit &e : list)
        edits.append({position(e.startLine, e.startColumn), position(e.endLine, e.endColumn), int(edits.size()), e.text});
    // Back to front keeps the earlier offsets valid. At the same start a longer range goes first (a deletion and an insertion at
    // its start: the insertion must land in front of what is left), and equal ranges in reverse order of the list, so inserts at
    // one position end up in list order, as the protocol demands.
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) {
        if (a.start != b.start)
            return a.start > b.start;
        if (a.end != b.end)
            return a.end > b.end;
        return a.index > b.index;
    });
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

bool CodeEditor::applyOffsetEdits(const QVector<MoveRefactor::TextEdit> &edits)
{
    if (isReadOnly() || edits.isEmpty())
        return false;
    QTextCursor c(document());
    c.beginEditBlock();
    for (int i = edits.size() - 1; i >= 0; --i) { // back to front keeps the earlier offsets valid
        const MoveRefactor::TextEdit &e = edits.at(i);
        c.setPosition(e.start);
        c.setPosition(e.start + e.length, QTextCursor::KeepAnchor);
        c.insertText(e.text);
    }
    c.endEditBlock();
    return true;
}

int CodeEditor::swatchGap() const
{
    return qMax(12, fontMetrics().height() - 2);
}

void CodeEditor::setColorSwatches(const QVector<ColorSwatch> &swatches)
{
    QVector<Swatch> next;
    next.reserve(swatches.size());
    for (const ColorSwatch &c : swatches) {
        const QTextBlock a = document()->findBlockByNumber(c.startLine), b = document()->findBlockByNumber(c.endLine);
        Swatch sw;
        if (!a.isValid() || !b.isValid()) { // keep the list aligned with the caller's
            next.append(sw);
            continue;
        }
        sw.start = QTextCursor(document());
        sw.start.setPosition(a.position() + qMin(c.startColumn, qMax(0, a.length() - 1)));
        sw.start.setKeepPositionOnInsert(true);
        sw.end = QTextCursor(document());
        sw.end.setPosition(b.position() + qMin(c.endColumn, qMax(0, b.length() - 1)));
        sw.color = c.color;
        next.append(sw);
    }
    // Blocks whose gap columns change (before or after) are highlighted again; the highlighter lays the gaps out.
    QSet<int> blocks;
    for (const Swatch &old : std::as_const(m_swatches))
        if (!old.start.isNull())
            blocks.insert(old.start.blockNumber());
    for (const Swatch &n : std::as_const(next))
        if (!n.start.isNull())
            blocks.insert(n.start.blockNumber());
    m_swatches = next;
    m_gapRevision = -1;
    applyGapProvider(blocks);
    viewport()->update();
}

QVector<QPair<int, int>> CodeEditor::gapColumns(const QTextBlock &block) const
{
    if (m_gapRevision != document()->revision()) {
        m_gapMap.clear();
        auto add = [this](int blockNumber, int column, int pixels) {
            QVector<QPair<int, int>> &list = m_gapMap[blockNumber];
            for (QPair<int, int> &g : list)
                if (g.first == column) { // a swatch and a hint in the same spot add up
                    g.second += pixels;
                    return;
                }
            list.append({column, pixels});
        };
        for (const Swatch &sw : m_swatches)
            if (!sw.start.isNull() && !sw.start.atBlockStart())
                add(sw.start.blockNumber(), sw.start.positionInBlock() - 1, swatchGap());
        for (const Hint &h : m_hints)
            if (!h.at.isNull() && !h.at.atBlockStart() && !h.at.atBlockEnd())
                add(h.at.blockNumber(), h.at.positionInBlock() - 1, h.width);
        m_gapRevision = document()->revision();
    }
    return m_gapMap.value(block.blockNumber());
}

void CodeEditor::applyGapProvider(const QSet<int> &blocks)
{
    auto *hl = document()->findChild<SyntaxHighlighter *>();
    if (!hl)
        return;
    hl->setGapProvider([this](const QTextBlock &b) { return gapColumns(b); });
    m_gapUpdating = true;
    const auto done = qScopeGuard([this] { m_gapUpdating = false; });
    for (const int n : blocks) {
        const QTextBlock b = document()->findBlockByNumber(n);
        if (b.isValid())
            hl->rehighlightBlock(b);
    }
}

QTextCursor CodeEditor::swatchRange(int index) const
{
    if (index < 0 || index >= m_swatches.size() || m_swatches.at(index).start.isNull())
        return QTextCursor();
    QTextCursor c(document());
    c.setPosition(m_swatches.at(index).start.position());
    c.setPosition(m_swatches.at(index).end.position(), QTextCursor::KeepAnchor);
    return c;
}

void CodeEditor::replaceSwatch(int index, const QString &text)
{
    QTextCursor c = swatchRange(index);
    if (c.isNull())
        return;
    c.beginEditBlock();
    c.insertText(text);
    c.endEditBlock();
}

void CodeEditor::paintSwatches()
{
    for (Swatch &sw : m_swatches)
        sw.rect = QRect();
    if (m_swatches.isEmpty())
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const int gap = swatchGap();
    const int size = qMax(8, gap - 4);
    const int viewBottom = viewport()->height();
    int firstBlock = 0, lastBlock = 0;
    visibleBlockRange(&firstBlock, &lastBlock); // cursorRect() lays a line out: only ask for the lines on screen
    for (Swatch &sw : m_swatches) {
        if (sw.start.isNull() || sw.start.atBlockStart() || !sw.start.block().isVisible())
            continue;
        const int bn = sw.start.blockNumber();
        if (bn < firstBlock || bn > lastBlock)
            continue;
        const QRect r = cursorRect(sw.start);
        if (r.bottom() < 0 || r.top() > viewBottom)
            continue;
        const QRect box(r.left() - gap + 2, r.top() + (r.height() - size) / 2, size, size);
        sw.rect = box.adjusted(-2, -2, 2, 2);
        // A checkerboard shows through translucent colours.
        QPainterPath path;
        path.addRoundedRect(QRectF(box), 3, 3);
        p.save();
        p.setClipPath(path);
        if (sw.color.alpha() < 255) {
            p.fillRect(box, QColor(255, 255, 255));
            p.fillRect(QRect(box.left(), box.top(), size / 2, size / 2), QColor(200, 200, 200));
            p.fillRect(QRect(box.left() + size / 2, box.top() + size / 2, size - size / 2, size - size / 2), QColor(200, 200, 200));
        }
        p.fillRect(box, sw.color);
        p.restore();
        p.setPen(QPen(t.border.lighter(t.dark ? 150 : 90), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
}

// The stock QPlainTextEdit menu has Qt's own icons; this one uses QODE's icon set.
void CodeEditor::contextMenuEvent(QContextMenuEvent *event)
{
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    auto icon = [&](const char *name) { return Icons::tinted(QStringLiteral(":/new-icons/%1.svg").arg(QLatin1String(name)), theme.editorFg, theme.textMuted); };
    const bool ro = isReadOnly();
    const bool sel = textCursor().hasSelection();
    QMenu menu(this);
    if (sel) {
        bool any = false;
        for (QAction *a : std::as_const(m_selectionActions)) {
            if (a && a->isEnabled()) {
                menu.addAction(a);
                any = true;
            }
        }
        if (any)
            menu.addSeparator();
    }
    auto add = [&](const char *ic, const QString &text, const QKeySequence &key, bool enabled, auto slot) {
        QAction *a = menu.addAction(icon(ic), text, this, slot);
        a->setShortcut(key);
        a->setShortcutVisibleInContextMenu(true);
        a->setEnabled(enabled);
    };
    add("undo", tr("Undo"), QKeySequence::Undo, !ro && document()->isUndoAvailable(), [this] {
        const QScopedValueRollback<bool> undoing(m_predictUndo, true);
        undo();
    });
    add("redo", tr("Redo"), QKeySequence::Redo, !ro && document()->isRedoAvailable(), [this] {
        const QScopedValueRollback<bool> undoing(m_predictUndo, true);
        redo();
    });
    menu.addSeparator();
    add("scissors", tr("Cut"), QKeySequence::Cut, !ro && sel, [this] { cut(); });
    add("copy", tr("Copy"), QKeySequence::Copy, sel, [this] { copy(); });
    add("clipboard-paste", tr("Paste"), QKeySequence::Paste, !ro && canPaste(), [this] { paste(); });
    add("circle-x", tr("Delete"), QKeySequence::Delete, !ro && sel, [this] { textCursor().removeSelectedText(); });
    menu.addSeparator();
    add("copy", tr("Duplicate Line"), QKeySequence(Qt::CTRL | Qt::Key_D), !ro, [this] { duplicateLine(); });
    menu.addSeparator();
    add("select-all", tr("Select All"), QKeySequence::SelectAll, !document()->isEmpty(), [this] { selectAll(); });
    bool separated = false;
    for (QAction *a : std::as_const(m_contextActions)) {
        if (!a || !a->isEnabled())
            continue;
        if (!separated)
            menu.addSeparator();
        separated = true;
        menu.addAction(a);
    }
    menu.exec(event->globalPos());
}
