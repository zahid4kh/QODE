#include "CodeEditor.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
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
#include <QTimer>
#include <QUrl>

namespace {

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *e) : QWidget(e), m_editor(e) {}
    QSize sizeHint() const override { return QSize(m_editor->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->lineNumberAreaPaintEvent(event); }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_editor->gutterClicked(event->pos());
    }

private:
    CodeEditor *m_editor;
};

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
    m_matchTimer = new QTimer(this);
    m_matchTimer->setSingleShot(true);
    m_matchTimer->setInterval(150);
    connect(m_matchTimer, &QTimer::timeout, this, &CodeEditor::recomputeMatches);

    setFrameShape(QFrame::NoFrame);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::refreshSelections);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_term.isEmpty())
            m_matchTimer->start();
        if (m_hasBase)
            m_diffTimer->start();
    });
    m_diffTimer = new QTimer(this);
    m_diffTimer->setSingleShot(true);
    m_diffTimer->setInterval(200);
    connect(m_diffTimer, &QTimer::timeout, this, &CodeEditor::recomputeDiff);

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
    m_matchBg = t.dark ? QColor(QStringLiteral("#614d1f")) : QColor(QStringLiteral("#f5e08a"));
    QPalette p = palette();
    p.setColor(QPalette::Base, t.editorBg);
    p.setColor(QPalette::Text, t.editorFg);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.editorFg);
    setPalette(p);
    m_lineArea->update();
    refreshSelections();
}

void CodeEditor::applySettings()
{
    auto &s = SettingsManager::instance();
    const QFont f = s.editorFont();
    setFont(f);
    setTabStopDistance(QFontMetricsF(f).horizontalAdvance(QLatin1Char(' ')) * s.tabSize());
    setLineWrapMode(s.wordWrap() ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
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
    return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
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

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(number == current ? m_gutterActive : m_gutterFg);
            painter.drawText(0, top, m_lineArea->width() - 8, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
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
    setExtraSelections(extra);
    m_lineArea->update();
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
    const bool opens = !trimmedBefore.isEmpty()
        && (trimmedBefore.endsWith(QLatin1Char('{')) || trimmedBefore.endsWith(QLatin1Char('(')) ||
            trimmedBefore.endsWith(QLatin1Char('[')) ||
            (m_indentAfterColon && trimmedBefore.endsWith(QLatin1Char(':'))));
    c.beginEditBlock();
    c.removeSelectedText();
    if (opens && !after.trimmed().isEmpty() && (after.trimmed().startsWith(QLatin1Char('}')) ||
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
    if (isReadOnly()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;

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
            handleBackspaceInIndent(event);
            return;
        }
        break;
    case Qt::Key_BraceRight:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
            // Typing '}' on a whitespace-only line dedents one level.
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

void CodeEditor::mousePressEvent(QMouseEvent *event)
{
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

void CodeEditor::gutterClicked(const QPoint &pos)
{
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
