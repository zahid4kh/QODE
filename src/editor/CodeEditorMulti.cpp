// Multiple cursors: extra carets beside the main one (textCursor()), Alt+click / Alt+drag, Alt+J occurrences.
#include "CodeEditor.h"
#include "CodeEditorText.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>

using namespace CodeEditorText;

namespace {

QString selectedPlain(const QTextCursor &c)
{
    QString t = c.selectedText();
    t.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    t.replace(QChar::LineSeparator, QLatin1Char('\n'));
    return t;
}

bool sameRange(const QTextCursor &a, const QTextCursor &b)
{
    return a.selectionStart() == b.selectionStart() && a.selectionEnd() == b.selectionEnd();
}

} // namespace

QList<QTextCursor> CodeEditor::allCursorsInOrder() const
{
    QList<QTextCursor> all = m_extraCursors;
    all.append(textCursor());
    std::stable_sort(all.begin(), all.end(), [](const QTextCursor &a, const QTextCursor &b) { return a.selectionStart() < b.selectionStart(); });
    return all;
}

void CodeEditor::setCursors(const QList<QTextCursor> &extras, const QTextCursor &main)
{
    const bool changed = extras.size() != m_extraCursors.size();
    m_extraCursors = extras;
    if (!m_extraCursors.isEmpty()) {
        hideCompletion();
        clearSnippet();
        if (hasPredictions())
            clearPredictions();
    }
    m_verticalX = -1; // addCursorVertically() sets it again after this
    setTextCursor(main); // refreshes the selections through cursorPositionChanged
    normalizeCursors();
    refreshSelections();
    viewport()->update();
    if (changed)
        emit cursorsChanged();
}

void CodeEditor::clearExtraCursors()
{
    if (m_extraCursors.isEmpty())
        return;
    m_extraCursors.clear();
    m_verticalX = -1;
    m_columnDrag.active = false;
    refreshSelections();
    viewport()->update();
    emit cursorsChanged();
}

// Carets that meet or overlap another one are merged into it; the main caret always survives.
void CodeEditor::normalizeCursors()
{
    if (m_extraCursors.isEmpty())
        return;
    struct Entry {
        QTextCursor c;
        int order; // index into m_extraCursors, -1 = the main caret
    };
    QVector<Entry> all;
    all.reserve(m_extraCursors.size() + 1);
    for (int i = 0; i < m_extraCursors.size(); ++i)
        all.append({m_extraCursors.at(i), i});
    all.append({textCursor(), -1});
    std::stable_sort(all.begin(), all.end(), [](const Entry &a, const Entry &b) { return a.c.selectionStart() < b.c.selectionStart(); });
    QVector<bool> drop(m_extraCursors.size(), false);
    bool any = false;
    Entry keep = all.first();
    for (int i = 1; i < all.size(); ++i) {
        const Entry &cur = all.at(i);
        const bool overlap = cur.c.selectionStart() < keep.c.selectionEnd() ||
                             (cur.c.selectionStart() == keep.c.selectionStart() && cur.c.selectionEnd() == keep.c.selectionEnd());
        if (!overlap) {
            keep = cur;
            continue;
        }
        // The main caret always stays; of two extra carets the newer one goes.
        const int victim = keep.order < 0 ? cur.order : cur.order < 0 ? keep.order : qMax(keep.order, cur.order);
        drop[victim] = true;
        any = true;
        if (victim == keep.order)
            keep = cur;
    }
    if (!any)
        return;
    QList<QTextCursor> kept;
    for (int i = 0; i < m_extraCursors.size(); ++i)
        if (!drop.at(i))
            kept.append(m_extraCursors.at(i));
    m_extraCursors = kept;
    if (m_extraCursors.isEmpty())
        m_verticalX = -1;
    emit cursorsChanged();
}

void CodeEditor::editAtCursors(const std::function<void(QTextCursor &, int)> &edit)
{
    const QScopedValueRollback<bool> own(m_predictApplying, true); // no edit predictions from a multi-caret edit
    QTextCursor main = textCursor();
    // Document order, so a paste can hand out one clipboard line per caret.
    QVector<QTextCursor *> order;
    order.reserve(m_extraCursors.size() + 1);
    for (QTextCursor &c : m_extraCursors)
        order.append(&c);
    order.append(&main);
    std::stable_sort(order.begin(), order.end(), [](const QTextCursor *a, const QTextCursor *b) { return a->selectionStart() < b->selectionStart(); });
    main.beginEditBlock();
    for (int i = 0; i < order.size(); ++i)
        edit(*order[i], i);
    main.endEditBlock();
    setTextCursor(main);
    normalizeCursors();
    ensureCursorVisible();
    refreshSelections();
    viewport()->update();
}

// --- Commands ---------------------------------------------------------------------------------------

void CodeEditor::addNextOccurrence()
{
    QTextCursor main = textCursor();
    if (!main.hasSelection()) {
        if (!m_extraCursors.isEmpty()) { // carets without selections: select the word at each
            QList<QTextCursor> extras = m_extraCursors;
            for (QTextCursor &c : extras)
                c.select(QTextCursor::WordUnderCursor);
            main.select(QTextCursor::WordUnderCursor);
            m_occurrenceWhole = true;
            setCursors(extras, main);
            return;
        }
        main.select(QTextCursor::WordUnderCursor);
        if (!main.hasSelection() || !isIdentifierChar(selectedPlain(main).at(0)))
            return;
        m_occurrenceWhole = true;
        m_occurrenceSeed = {main.selectionStart(), main.selectionEnd()};
        setTextCursor(main);
        return;
    }
    if (m_extraCursors.isEmpty() && m_occurrenceSeed != qMakePair(main.selectionStart(), main.selectionEnd()))
        m_occurrenceWhole = false; // the user's own selection: any occurrence, inside words too
    const QString needle = selectedPlain(main);
    if (needle.contains(QLatin1Char('\n')))
        return; // QTextDocument::find() does not match across lines
    QTextDocument::FindFlags flags = QTextDocument::FindCaseSensitively;
    if (m_occurrenceWhole)
        flags |= QTextDocument::FindWholeWords;
    const QList<QTextCursor> all = allCursorsInOrder();
    auto taken = [&all](const QTextCursor &f) {
        for (const QTextCursor &c : all)
            if (sameRange(c, f))
                return true;
        return false;
    };
    QTextCursor found = document()->find(needle, main.selectionEnd(), flags);
    if (found.isNull() || taken(found)) {
        // Wrap around, skipping the occurrences that already have a caret.
        found = document()->find(needle, 0, flags);
        while (!found.isNull() && taken(found))
            found = document()->find(needle, found.selectionEnd(), flags);
    }
    if (found.isNull())
        return;
    QList<QTextCursor> extras = m_extraCursors;
    extras.append(main);
    setCursors(extras, found);
    centerCursor();
}

void CodeEditor::removeLastOccurrence()
{
    if (m_extraCursors.isEmpty())
        return;
    QList<QTextCursor> extras = m_extraCursors;
    const QTextCursor main = extras.takeLast();
    setCursors(extras, main);
    ensureCursorVisible();
}

void CodeEditor::selectAllOccurrences()
{
    QTextCursor main = textCursor();
    if (!main.hasSelection()) {
        main.select(QTextCursor::WordUnderCursor);
        if (!main.hasSelection() || !isIdentifierChar(selectedPlain(main).at(0)))
            return;
        m_occurrenceWhole = true;
    } else if (m_extraCursors.isEmpty() && m_occurrenceSeed != qMakePair(main.selectionStart(), main.selectionEnd())) {
        m_occurrenceWhole = false;
    }
    const QString needle = selectedPlain(main);
    if (needle.isEmpty() || needle.contains(QLatin1Char('\n')))
        return;
    QTextDocument::FindFlags flags = QTextDocument::FindCaseSensitively;
    if (m_occurrenceWhole)
        flags |= QTextDocument::FindWholeWords;
    QList<QTextCursor> extras;
    for (QTextCursor f = document()->find(needle, 0, flags); !f.isNull(); f = document()->find(needle, f.selectionEnd(), flags)) {
        if (!sameRange(f, main))
            extras.append(f);
        if (extras.size() >= 10000)
            break;
    }
    setCursors(extras, main);
}

void CodeEditor::addCursorVertically(bool down)
{
    const QList<QTextCursor> all = allCursorsInOrder();
    QTextCursor from = down ? all.last() : all.first();
    from.clearSelection();
    if (m_verticalX < 0) {
        from = textCursor();
        from.clearSelection();
    }
    QTextCursor to = from;
    if (m_verticalX >= 0)
        to.setVerticalMovementX(m_verticalX);
    if (!to.movePosition(down ? QTextCursor::Down : QTextCursor::Up) || to.position() == from.position())
        return;
    const int x = to.verticalMovementX();
    QList<QTextCursor> extras = m_extraCursors;
    extras.append(textCursor());
    setCursors(extras, to);
    m_verticalX = x;
    ensureCursorVisible();
}

void CodeEditor::addCursorsToLineEnds()
{
    const QTextCursor main = textCursor();
    const QTextBlock first = document()->findBlock(main.selectionStart());
    const QTextBlock last = document()->findBlock(main.selectionEnd());
    if (first == last)
        return;
    QList<QTextCursor> extras;
    QTextCursor end;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        QTextCursor c(b);
        c.movePosition(QTextCursor::EndOfBlock);
        if (b == last) {
            if (main.selectionEnd() == b.position()) // the selection stops at the start of this line: leave it out
                break;
            end = c;
            break;
        }
        extras.append(c);
    }
    if (end.isNull()) {
        if (extras.isEmpty())
            return;
        end = extras.takeLast();
    }
    setCursors(extras, end);
}

// --- Keys ----------------------------------------------------------------------------------------------

bool CodeEditor::multiCursorShortcut(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const Qt::KeyboardModifiers ca = Qt::ControlModifier | Qt::AltModifier;
    switch (event->key()) {
    case Qt::Key_J:
        if (mods == Qt::AltModifier) {
            addNextOccurrence();
            return true;
        }
        if (mods == (Qt::AltModifier | Qt::ShiftModifier)) {
            removeLastOccurrence();
            return true;
        }
        if (mods == (ca | Qt::ShiftModifier)) {
            selectAllOccurrences();
            return true;
        }
        break;
    case Qt::Key_L:
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
            selectAllOccurrences();
            return true;
        }
        break;
    case Qt::Key_I:
        if (mods == (Qt::AltModifier | Qt::ShiftModifier)) {
            addCursorsToLineEnds();
            return true;
        }
        break;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods == ca || mods == (Qt::AltModifier | Qt::ShiftModifier)) {
            addCursorVertically(event->key() == Qt::Key_Down);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

bool CodeEditor::multiCursorKey(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    switch (event->key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return false; // a modifier on its way to a combination
    case Qt::Key_Escape:
        clearExtraCursors();
        return true;
    default:
        break;
    }
    if (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo) || event->matches(QKeySequence::SelectAll)) {
        clearExtraCursors(); // undo puts back the main caret only
        return false;
    }
    if (event->matches(QKeySequence::Copy)) {
        copyAtCursors(false);
        return true;
    }
    const bool ro = isReadOnly();
    if (event->matches(QKeySequence::Cut)) {
        if (!ro)
            copyAtCursors(true);
        return true;
    }
    if (event->matches(QKeySequence::Paste)) {
        if (!ro)
            pasteAtCursors();
        return true;
    }

    // Caret movement: every caret moves the same way.
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const QTextCursor::MoveMode mode = shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
    auto moveAll = [&](const std::function<void(QTextCursor &)> &move) {
        QList<QTextCursor> extras = m_extraCursors;
        for (QTextCursor &c : extras)
            move(c);
        QTextCursor main = textCursor();
        move(main);
        m_extraCursors = extras;
        setTextCursor(main);
        normalizeCursors();
        ensureCursorVisible();
        refreshSelections();
        viewport()->update();
    };
    if (!(mods & (Qt::AltModifier | Qt::MetaModifier))) {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right: {
            const bool left = event->key() == Qt::Key_Left;
            moveAll([&](QTextCursor &c) {
                if (c.hasSelection() && !shift && !ctrl) { // collapse to the side of the arrow
                    c.setPosition(left ? c.selectionStart() : c.selectionEnd());
                    return;
                }
                c.movePosition(ctrl ? (left ? QTextCursor::PreviousWord : QTextCursor::NextWord)
                                    : (left ? QTextCursor::PreviousCharacter : QTextCursor::NextCharacter),
                               mode);
            });
            return true;
        }
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (ctrl)
                break;
            moveAll([&](QTextCursor &c) { c.movePosition(event->key() == Qt::Key_Up ? QTextCursor::Up : QTextCursor::Down, mode); });
            return true;
        case Qt::Key_Home:
            if (ctrl)
                break;
            moveAll([&](QTextCursor &c) {
                // Smart home: the first non-blank character, then the start of the line.
                const QString text = c.block().text();
                int indent = 0;
                while (indent < text.size() && text.at(indent).isSpace())
                    ++indent;
                const int target = c.positionInBlock() == indent ? 0 : indent;
                c.setPosition(c.block().position() + target, mode);
            });
            return true;
        case Qt::Key_End:
            if (ctrl)
                break;
            moveAll([&](QTextCursor &c) { c.movePosition(QTextCursor::EndOfBlock, mode); });
            return true;
        default:
            break;
        }
    }

    if (ro) {
        if (mods & (Qt::ControlModifier | Qt::AltModifier) || event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown) {
            clearExtraCursors();
            return false;
        }
        return true; // typing does nothing in a read-only file
    }

    // Edits.
    switch (event->key()) {
    case Qt::Key_Backspace:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier || mods == Qt::ControlModifier) {
            editAtCursors([ctrl](QTextCursor &c, int) {
                if (!c.hasSelection())
                    c.movePosition(ctrl ? QTextCursor::PreviousWord : QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
                c.removeSelectedText();
            });
            return true;
        }
        break;
    case Qt::Key_Delete:
        if (mods == Qt::NoModifier || mods == Qt::ControlModifier) {
            editAtCursors([ctrl](QTextCursor &c, int) {
                if (!c.hasSelection())
                    c.movePosition(ctrl ? QTextCursor::EndOfWord : QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                c.removeSelectedText();
            });
            return true;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
            editAtCursors([](QTextCursor &c, int) {
                c.removeSelectedText();
                const QString text = c.block().text();
                int indent = 0;
                while (indent < text.size() && indent < c.positionInBlock() && (text.at(indent) == QLatin1Char(' ') || text.at(indent) == QLatin1Char('\t')))
                    ++indent;
                c.insertText(QLatin1Char('\n') + text.left(indent));
            });
            return true;
        }
        break;
    case Qt::Key_Tab:
        if (mods == Qt::NoModifier) {
            const QString unit = indentUnit();
            editAtCursors([&unit](QTextCursor &c, int) {
                if (unit.startsWith(QLatin1Char(' '))) {
                    const int col = c.selectionStart() - c.document()->findBlock(c.selectionStart()).position();
                    c.insertText(QString(unit.size() - (col % unit.size()), QLatin1Char(' ')));
                } else {
                    c.insertText(QStringLiteral("\t"));
                }
            });
            return true;
        }
        break;
    case Qt::Key_Backtab: {
        const QString unit = indentUnit();
        editAtCursors([&unit](QTextCursor &c, int) {
            QTextCursor line(c.block());
            const QString text = c.block().text();
            int remove = 0;
            if (text.startsWith(QLatin1Char('\t')))
                remove = 1;
            else
                while (remove < unit.size() && remove < text.size() && text.at(remove) == QLatin1Char(' '))
                    ++remove;
            line.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, remove);
            line.removeSelectedText();
        });
        return true;
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        clearExtraCursors();
        return false;
    default:
        break;
    }

    // Typed text. AltGr arrives as Ctrl+Alt on some layouts, so only a lone Ctrl or Alt marks a shortcut.
    const QString text = event->text();
    const bool withCtrl = mods & Qt::ControlModifier, withAlt = mods & Qt::AltModifier;
    const bool shortcut = withCtrl != withAlt || (mods & Qt::MetaModifier);
    if (!text.isEmpty() && !shortcut && text.at(0).isPrint()) {
        const QChar ch = text.size() == 1 ? text.at(0) : QChar();
        const bool wrapper = isOpeningBracket(ch) || ch == QLatin1Char('"') || ch == QLatin1Char('\'') || ch == QLatin1Char('`');
        editAtCursors([&](QTextCursor &c, int) {
            if (wrapper && c.hasSelection()) { // wrap each selection, keeping it selected
                const int s = c.selectionStart(), e = c.selectionEnd();
                const QChar close = isOpeningBracket(ch) ? partnerOf(ch) : ch;
                QTextCursor w(c.document());
                w.setPosition(e);
                w.insertText(QString(close));
                w.setPosition(s);
                w.insertText(QString(ch));
                c.setPosition(s + 1);
                c.setPosition(e + 1, QTextCursor::KeepAnchor);
                return;
            }
            c.insertText(text);
        });
        return true;
    }
    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        // Any other shortcut works on the main caret alone.
        clearExtraCursors();
        return false;
    }
    return true;
}

// --- Clipboard ----------------------------------------------------------------------------------------

void CodeEditor::copyAtCursors(bool cut)
{
    const QList<QTextCursor> all = allCursorsInOrder();
    QStringList pieces;
    bool any = false;
    for (const QTextCursor &c : all) {
        pieces << selectedPlain(c);
        any = any || c.hasSelection();
    }
    if (!any)
        return;
    m_multiClip = pieces;
    m_multiClipText = pieces.join(QLatin1Char('\n'));
    QApplication::clipboard()->setText(m_multiClipText);
    if (cut)
        editAtCursors([](QTextCursor &c, int) { c.removeSelectedText(); });
}

void CodeEditor::pasteAtCursors()
{
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    const int count = cursorCount();
    QStringList parts;
    if (text == m_multiClipText && m_multiClip.size() == count) {
        parts = m_multiClip;
    } else {
        QStringList lines = text.split(QLatin1Char('\n'));
        if (lines.size() > 1 && lines.last().isEmpty())
            lines.removeLast();
        if (lines.size() == count)
            parts = lines; // one line per caret
    }
    editAtCursors([&](QTextCursor &c, int i) { c.insertText(parts.isEmpty() ? text : parts.at(i)); });
}

// --- Mouse -----------------------------------------------------------------------------------------------

void CodeEditor::toggleCursorAt(const QPoint &viewportPos)
{
    const QTextCursor at = cursorForPosition(viewportPos);
    const int pos = at.position();
    QList<QTextCursor> extras = m_extraCursors;
    for (int i = 0; i < extras.size(); ++i) {
        const QTextCursor &c = extras.at(i);
        if (c.position() == pos || (c.hasSelection() && pos > c.selectionStart() && pos < c.selectionEnd())) {
            extras.removeAt(i);
            setCursors(extras, textCursor());
            return;
        }
    }
    const QTextCursor main = textCursor();
    if (!extras.isEmpty() && (main.position() == pos || (main.hasSelection() && pos > main.selectionStart() && pos < main.selectionEnd()))) {
        const QTextCursor next = extras.takeLast();
        setCursors(extras, next);
        return;
    }
    extras.append(main);
    setCursors(extras, at);
}

// Alt+drag: a box from the press point to `viewportPos`, one caret per line.
void CodeEditor::columnSelect(const QPoint &viewportPos)
{
    const qreal offset = contentOffset().x();
    const qreal x0 = m_columnDrag.anchorX + offset, x1 = viewportPos.x();
    const QTextBlock anchor = document()->findBlockByNumber(m_columnDrag.anchorBlock);
    const QTextBlock current = cursorForPosition(viewportPos).block();
    if (!anchor.isValid() || !current.isValid())
        return;
    const bool downwards = current.blockNumber() >= anchor.blockNumber();
    auto at = [this](const QTextBlock &b, qreal x) {
        const qreal y = blockBoundingGeometry(b).translated(contentOffset()).top() + 1;
        return cursorForPosition(QPoint(qRound(x), qRound(y)));
    };
    QList<QTextCursor> rows;
    for (QTextBlock b = anchor;; b = downwards ? b.next() : b.previous()) {
        if (!b.isValid())
            break;
        if (b.isVisible()) {
            QTextCursor c = at(b, x0);
            const QTextCursor end = at(b, x1);
            const bool box = qAbs(x1 - x0) > 1;
            // A line too short to reach the box gets no caret (unless the box is a single column).
            QTextCursor lineEnd(b);
            lineEnd.movePosition(QTextCursor::EndOfBlock);
            const qreal endX = cursorRect(lineEnd).left();
            if (!box || endX >= qMin(x0, x1) - 1) {
                c.setPosition(end.position(), QTextCursor::KeepAnchor);
                rows.append(c);
            }
        }
        if (b == current)
            break;
    }
    if (rows.isEmpty())
        return;
    const QTextCursor main = rows.takeLast();
    m_extraCursors = rows;
    setTextCursor(main);
    refreshSelections();
    viewport()->update();
    emit cursorsChanged();
}

// --- Painting --------------------------------------------------------------------------------------------

void CodeEditor::appendCursorSelections(QList<QTextEdit::ExtraSelection> &extra) const
{
    if (m_extraCursors.isEmpty())
        return;
    QTextCharFormat f;
    f.setBackground(palette().brush(QPalette::Highlight));
    f.setForeground(palette().brush(QPalette::HighlightedText));
    for (const QTextCursor &c : m_extraCursors) {
        if (!c.hasSelection())
            continue;
        QTextEdit::ExtraSelection sel;
        sel.format = f;
        sel.cursor = c;
        extra.append(sel);
    }
}

void CodeEditor::paintExtraCursors()
{
    if (m_extraCursors.isEmpty())
        return;
    QPainter p(viewport());
    const QRect visible = viewport()->rect();
    const QColor color = palette().color(QPalette::Text);
    for (const QTextCursor &c : std::as_const(m_extraCursors)) {
        if (!c.block().isVisible())
            continue;
        const QRect r = cursorRect(c);
        if (r.intersects(visible))
            p.fillRect(QRect(r.left(), r.top(), qMax(1, cursorWidth()), r.height()), color);
    }
}
