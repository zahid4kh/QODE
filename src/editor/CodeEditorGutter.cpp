// The gutter's per-line state: git change markers and hunks, blame, bookmarks, breakpoints and the execution line, run
// markers, and what a click on them does.
#include "CodeEditor.h"

#include "CodeEditorText.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

using namespace CodeEditorText;

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
void CodeEditor::trackLineEdit(int position, int, int added)
{
    const int now = document()->blockCount();
    const int delta = now - m_blameBlocks;
    m_blameBlocks = now;
    const QTextBlock editedBlock = document()->findBlock(position);
    // A line break typed in front of a line pushes that whole line down: it moves, the line above stays.
    if (delta > 0 && added > 0 && position == editedBlock.position())
        shiftServerData(editedBlock.blockNumber() - 1, delta);
    else
        shiftServerData(editedBlock.blockNumber(), delta);
    if (delta == 0 && m_blame.isEmpty())
        return;
    const int edited = editedBlock.blockNumber();
    if (delta != 0 && (!m_breakpoints.isEmpty() || m_execLine > edited)) {
        QList<int> moved;
        for (int l : std::as_const(m_breakpoints)) {
            if (l <= edited)
                moved.append(l);
            else if (delta > 0 || l > edited - delta)
                moved.append(l + delta);
        }
        if (m_execLine > edited)
            m_execLine = delta > 0 || m_execLine > edited - delta ? m_execLine + delta : -1;
        if (moved != m_breakpoints) {
            m_breakpoints = moved;
            m_lineArea->update();
            emit breakpointsChanged();
        }
    }
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

void CodeEditor::setBreakpointsEnabled(bool on)
{
    if (m_breakpointsOn == on)
        return;
    m_breakpointsOn = on;
    m_lineArea->update();
}

void CodeEditor::setBreakpoints(const QList<int> &lines)
{
    QList<int> sorted = lines;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted == m_breakpoints)
        return;
    m_breakpoints = sorted;
    refreshSelections();
}

void CodeEditor::toggleBreakpoint(int line)
{
    if (line < 0)
        line = textCursor().blockNumber();
    if (m_breakpoints.contains(line)) {
        m_breakpoints.removeOne(line);
    } else {
        m_breakpoints.append(line);
        std::sort(m_breakpoints.begin(), m_breakpoints.end());
    }
    refreshSelections();
    emit breakpointsChanged();
}

void CodeEditor::setExecutionLine(int line)
{
    if (line == m_execLine)
        return;
    m_execLine = line;
    refreshSelections();
    if (line >= 0) {
        const QTextBlock b = document()->findBlockByNumber(line);
        if (b.isValid() && !b.isVisible())
            unfoldAll();
        QTextCursor c(b);
        setTextCursor(c);
        centerCursor();
    }
}

// The line numbers themselves (between the light bulb column and the fold markers).
bool CodeEditor::breakpointAreaAt(const QPoint &pos) const
{
    if (!m_breakpointsOn)
        return false;
    const int left = blameWidth() + (m_bulbEnabled || !m_runLines.isEmpty() ? kBulbWidth + 4 : 8);
    return pos.x() >= left && pos.x() < m_lineArea->width() - kFoldWidth;
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
    const int space = fontMetrics().horizontalAdvance(QLatin1Char(' '));
    const int x = qMax(r.right(), m_lensEnd.value(line, 0)) + space * 4; // after the code lenses of the line
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

void CodeEditor::setRunMarkers(const QList<int> &lines, const QString &tooltip)
{
    if (lines == m_runLines && tooltip == m_runTip)
        return;
    const bool widthChanged = lines.isEmpty() != m_runLines.isEmpty();
    m_runLines = lines;
    m_runTip = tooltip;
    if (widthChanged)
        updateLineNumberAreaWidth();
    m_lineArea->update();
}

bool CodeEditor::runMarkerAt(const QPoint &pos, int *line) const
{
    if (m_runLines.isEmpty() || pos.x() < blameWidth() || pos.x() > blameWidth() + kBulbWidth + 4)
        return false;
    const int at = cursorForPosition(QPoint(0, pos.y())).blockNumber();
    if (!m_runLines.contains(at) || (m_bulbEnabled && at == m_bulbLine))
        return false;
    if (line)
        *line = at;
    return true;
}

void CodeEditor::paintRunMarker(QPainter &painter, int top, int lineHeight)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const int size = 11;
    painter.drawPixmap(blameWidth() + 5, top + (lineHeight - size) / 2, Icons::pixmap(QStringLiteral(":/new-icons/play.svg"), t.success, size));
}

void CodeEditor::gutterClicked(const QPoint &pos)
{
    if (int line = -1; runMarkerAt(pos, &line)) {
        emit runRequested(line);
        return;
    }
    if (m_bulbEnabled && m_bulbLine >= 0 && m_bulbRect.adjusted(-2, -2, 2, 2).contains(pos) &&
        cursorForPosition(QPoint(0, pos.y())).blockNumber() == m_bulbLine) {
        requestCodeActions();
        return;
    }
    if (breakpointAreaAt(pos)) {
        const QTextBlock b = cursorForPosition(QPoint(0, pos.y())).block();
        if (b.isValid())
            toggleBreakpoint(b.blockNumber());
        return;
    }
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
        view->setProperty("qodeZoomed", true); // the height below comes from the zoomed font already
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
