// Folding: what can fold (brackets, indentation, import runs, comment blocks), hiding and showing the lines, the
// gutter's fold markers.
#include "CodeEditor.h"

#include "CodeEditorText.h"
#include "MiniMap.h"

#include <QFileInfo>
#include <QPainter>
#include <QPlainTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

using namespace CodeEditorText;

namespace {

// Per-block fold flag (a QTextBlock owns its user data).
struct FoldData : QTextBlockUserData {
    bool folded = false;
};

} // namespace

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
    if (serverFoldEnd(block) >= 0)
        return true;

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
    if (const int server = serverFoldEnd(header); server >= 0)
        return server;
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

bool CodeEditor::hasBlockComments(const QString &path)
{
    static const QStringList exts = {QStringLiteral("kt"),   QStringLiteral("kts"),  QStringLiteral("java"), QStringLiteral("c"),
                                     QStringLiteral("h"),    QStringLiteral("cpp"),  QStringLiteral("cc"),   QStringLiteral("cxx"),
                                     QStringLiteral("hpp"),  QStringLiteral("hh"),   QStringLiteral("hxx"),  QStringLiteral("js"),
                                     QStringLiteral("jsx"),  QStringLiteral("mjs"),  QStringLiteral("cjs"),  QStringLiteral("ts"),
                                     QStringLiteral("tsx"),  QStringLiteral("go"),   QStringLiteral("rs"),   QStringLiteral("swift"),
                                     QStringLiteral("scala"), QStringLiteral("groovy"), QStringLiteral("gradle"), QStringLiteral("dart"),
                                     QStringLiteral("cs"),   QStringLiteral("php"),  QStringLiteral("css"),  QStringLiteral("scss"),
                                     QStringLiteral("less")};
    return exts.contains(QFileInfo(path).suffix().toLower());
}

void CodeEditor::foldComments(bool header, bool docs)
{
    if (!header && !docs)
        return;
    const int caret = textCursor().blockNumber();
    bool seenCode = false;
    bool changed = false;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const QString t = b.text().trimmed();
        if (t.isEmpty())
            continue;
        if (t.startsWith(QLatin1String("#!"))) // a script's shebang comes before its header
            continue;
        if (!t.startsWith(QLatin1String("/*")) || t.indexOf(QLatin1String("*/"), 2) >= 0) {
            seenCode = true;
            continue;
        }
        const QTextBlock start = b;
        int endBlock = -1;
        for (QTextBlock e = b.next(); e.isValid(); e = e.next())
            if (e.text().contains(QLatin1String("*/"))) {
                endBlock = e.blockNumber();
                b = e;
                break;
            }
        if (endBlock < 0)
            break; // an unclosed comment: nothing after it is code
        const bool isHeader = !seenCode;
        const bool isDoc = t.startsWith(QLatin1String("/**")) && !t.startsWith(QLatin1String("/***"));
        seenCode = true;
        const bool want = isHeader ? header : (docs && isDoc && endBlock - start.blockNumber() >= 2);
        if (!want || isFolded(start) || (caret > start.blockNumber() && caret <= endBlock))
            continue;
        if (isFoldable(start) && foldEnd(start) > start.blockNumber()) {
            setFolded(start, true);
            changed = true;
        }
    }
    if (changed)
        applyFolds();
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

bool CodeEditor::foldIconAt(const QPoint &pos) const
{
    if (pos.x() < m_lineArea->width() - kFoldWidth)
        return false;
    const QTextBlock b = cursorForPosition(QPoint(0, pos.y())).block();
    return b.isValid() && (isFolded(b) || isFoldable(b));
}
