// The editor side of next edit predictions (EditPredict works them out from the text): follows the user's edit as a
// session, asks EditPredict where else it belongs once typing pauses, marks those places, and applies them on Tab.
#include "CodeEditor.h"

#include "EditPredict.h"
#include "Language.h"
#include "SyntaxHighlighter.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QHash>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <memory>

namespace {
constexpr int kPredictDelay = 250;          // ms of quiet after an edit before the predictions are worked out
constexpr int kPredictMaxText = 2'000'000; // characters; bigger documents are not scanned
constexpr int kPillPad = 3;  // between the pill edge and the Tab chip
constexpr int kPillGap = 10; // between the sections inside the pill

QString plain(QString s)
{
    s.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    s.replace(QChar::Nbsp, QLatin1Char(' ')); // as QTextDocument::toPlainText() does
    return s;
}

QString rangeText(QTextDocument *doc, int from, int to)
{
    QTextCursor c(doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    return plain(c.selectedText());
}

// TokenRole of each character as the syntax highlighter left it, one block at a time (-1 for plain code).
class RoleLookup
{
public:
    explicit RoleLookup(const QTextDocument *doc) : m_doc(doc) {}
    int at(int pos)
    {
        const QTextBlock block = m_doc->findBlock(pos);
        if (!block.isValid())
            return -1;
        auto it = m_masks.find(block.blockNumber());
        if (it == m_masks.end()) {
            QVector<qint8> mask(block.length(), -1);
            for (const QTextLayout::FormatRange &r : block.layout()->formats()) {
                const QVariant role = r.format.property(kTokenRoleProperty);
                if (!role.isValid())
                    continue;
                for (int i = r.start; i < r.start + r.length && i < mask.size(); ++i)
                    mask[i] = qint8(role.toInt());
            }
            it = m_masks.insert(block.blockNumber(), mask);
        }
        const int i = pos - block.position();
        return i >= 0 && i < it->size() ? it->at(i) : -1;
    }

private:
    const QTextDocument *m_doc;
    QHash<int, QVector<qint8>> m_masks;
};
} // namespace

void CodeEditor::setupPredictions()
{
    m_predictTimer = new QTimer(this);
    m_predictTimer->setSingleShot(true);
    m_predictTimer->setInterval(kPredictDelay);
    connect(m_predictTimer, &QTimer::timeout, this, &CodeEditor::computePredictions);
}

bool CodeEditor::predictionsWanted() const
{
    static const QStringList prose = {QStringLiteral("Markdown"), QStringLiteral("CSV"), QStringLiteral("TSV"),
                                      QStringLiteral("JSON"),     QStringLiteral("HTML"), QStringLiteral("XML"),
                                      QStringLiteral("CSS")};
    return m_predictEnabled && !isReadOnly() && !prose.contains(m_language) && document()->characterCount() < kPredictMaxText;
}

void CodeEditor::resetPredictionText()
{
    m_shadow = document()->toPlainText();
    m_predictSession = {};
    clearPredictions();
}

void CodeEditor::clearPredictions()
{
    m_acceptedFrom = m_acceptedTo = QTextCursor();
    if (m_predictions.isEmpty())
        return;
    m_predictions.clear();
    m_predictPill = QRect();
    refreshSelections();
    viewport()->update();
}

void CodeEditor::predictionChange(int pos, int removed, int added)
{
    // Qt may count the final paragraph separator in `removed`: take the real size difference from the document.
    const int length = document()->characterCount() - 1;
    if (pos < 0 || pos > m_shadow.size()) {
        resetPredictionText();
        return;
    }
    removed = qMin(removed, int(m_shadow.size()) - pos);
    added = length - (int(m_shadow.size()) - removed);
    if (added < 0 || pos + added > length) {
        resetPredictionText();
        return;
    }
    const QString addedText = rangeText(document(), pos, pos + added);
    const QString removedText = m_shadow.mid(pos, removed);
    if (addedText == removedText)
        return; // only formats changed (highlighting)
    const QString shadowBefore = m_shadow;
    m_shadow.replace(pos, removed, addedText);
    const int delta = added - removed;
    PredictSession &s = m_predictSession;

    if (m_predictApplying) { // our own edit: the session's text stays what it is, only its place may move
        if (s.start >= 0) {
            if (pos + removed <= s.start) {
                s.start += delta;
                s.end += delta;
            } else if (pos < s.end) {
                s = {};
            }
        }
        return;
    }
    if (!m_acceptedFrom.isNull()) {
        if (pos >= m_acceptedFrom.position() && pos + added <= m_acceptedTo.position())
            return; // typing over a placeholder argument that was just inserted
        m_acceptedFrom = m_acceptedTo = QTextCursor();
    }
    if (!hasFocus() || m_predictUndo || m_snippetAt >= 0 || !predictionsWanted()) {
        // Undo, an agent or formatter edit, a snippet being filled in: not something to repeat elsewhere.
        s = {};
        if (!m_predictions.isEmpty() && (m_predictUndo || !hasFocus()))
            clearPredictions();
        return;
    }
    if (s.start >= 0 && pos <= s.end && pos + removed >= s.start) {
        // Next to (or inside) the current edit: it grows, and so does what it replaced.
        const int start = qMin(s.start, pos), end = qMax(s.end, pos + removed);
        s.before = shadowBefore.mid(start, s.start - start) + s.before + shadowBefore.mid(s.end, end - s.end);
        s.start = start;
        s.end = end + delta;
    } else {
        s.start = pos;
        s.end = pos + added;
        s.before = removedText;
        clearPredictions(); // an edit somewhere else ends the old suggestions
    }
    s.dirty = true;
    m_predictTimer->start();
}

void CodeEditor::computePredictions()
{
    PredictSession &s = m_predictSession;
    if (!s.dirty)
        return;
    s.dirty = false;
    const QString text = document()->toPlainText();
    if (s.start < 0 || s.end > text.size() || text.size() != m_shadow.size() || !predictionsWanted()) {
        clearPredictions();
        return;
    }
    auto roles = std::make_shared<RoleLookup>(document());
    EditPredict::Context ctx;
    ctx.language = m_language;
    ctx.inString = [roles](int i) { return roles->at(i) == int(TokenRole::String); };
    ctx.inComment = [roles](int i) { return roles->at(i) == int(TokenRole::Comment); };
    const EditPredict::Prediction p = EditPredict::predict(text, {s.start, s.before, text.mid(s.start, s.end - s.start)}, ctx);

    m_predictions.clear();
    for (const EditPredict::Edit &e : p.edits) {
        PendingEdit pe;
        pe.from = QTextCursor(document());
        pe.from.setPosition(e.start);
        pe.to = QTextCursor(document());
        pe.to.setPosition(e.end);
        pe.expect = text.mid(e.start, e.end - e.start);
        pe.text = e.text;
        pe.selectFrom = e.selectFrom;
        pe.selectLength = e.selectLength;
        m_predictions.append(pe);
    }
    m_predictKind = int(p.kind);
    m_predictFrom = p.from;
    m_predictTo = p.to;
    m_predictTotal = m_predictions.size();
    m_predictPill = QRect();
    refreshSelections();
    viewport()->update();
}

int CodeEditor::nextPrediction() const
{
    const int caret = textCursor().position();
    for (int i = 0; i < m_predictions.size(); ++i)
        if (m_predictions.at(i).to.position() >= caret)
            return i;
    return 0; // wrap around to the first one
}

bool CodeEditor::acceptPrediction()
{
    if (m_predictSession.dirty)
        computePredictions();
    // Places the user changed by hand in the meantime are dropped.
    for (int i = m_predictions.size() - 1; i >= 0; --i) {
        const PendingEdit &e = m_predictions.at(i);
        if (rangeText(document(), e.from.position(), e.to.position()) != e.expect)
            m_predictions.removeAt(i);
    }
    if (m_predictions.isEmpty()) {
        clearPredictions();
        return false;
    }
    const int i = nextPrediction();
    const PendingEdit e = m_predictions.at(i);
    const int start = e.from.position(), end = e.to.position();

    // Off screen: the first Tab only goes there, so the change is seen before it is made.
    QTextCursor at(document());
    at.setPosition(start);
    const QRect r = cursorRect(at);
    if (!at.block().isVisible() || !viewport()->rect().contains(r.center())) {
        setTextCursor(at);
        scrollBlockToCenter(at.blockNumber());
        refreshSelections();
        viewport()->update();
        return true;
    }

    QTextCursor c(document());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    m_predictApplying = true;
    c.beginEditBlock();
    c.insertText(e.text);
    c.endEditBlock();
    m_predictApplying = false;
    m_predictions.removeAt(i);

    QTextCursor caret(document());
    if (e.selectFrom >= 0) {
        caret.setPosition(start + e.selectFrom);
        caret.setPosition(start + e.selectFrom + e.selectLength, QTextCursor::KeepAnchor);
        m_acceptedFrom = QTextCursor(document());
        m_acceptedFrom.setPosition(start);
        m_acceptedTo = QTextCursor(document());
        m_acceptedTo.setPosition(start + e.text.size());
    } else {
        caret.setPosition(start + e.text.size());
    }
    setTextCursor(caret);
    ensureCursorVisible();
    m_predictPill = QRect();
    refreshSelections();
    viewport()->update();
    return true;
}

void CodeEditor::acceptAllPredictions()
{
    if (m_predictSession.dirty)
        computePredictions();
    if (m_predictions.isEmpty())
        return;
    QTextCursor c(document());
    c.setPosition(m_predictions.first().from.position()); // undo puts the caret where the block began
    m_predictApplying = true;
    c.beginEditBlock();
    for (int i = m_predictions.size() - 1; i >= 0; --i) { // from the end, so earlier positions stay put
        const PendingEdit &e = m_predictions.at(i);
        if (rangeText(document(), e.from.position(), e.to.position()) != e.expect)
            continue;
        c.setPosition(e.from.position());
        c.setPosition(e.to.position(), QTextCursor::KeepAnchor);
        c.insertText(e.text);
    }
    c.endEditBlock();
    m_predictApplying = false;
    clearPredictions();
}

void CodeEditor::appendPredictionSelections(QList<QTextEdit::ExtraSelection> &extra) const
{
    if (m_predictions.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const int next = nextPrediction();
    for (int i = 0; i < m_predictions.size(); ++i) {
        const PendingEdit &e = m_predictions.at(i);
        if (e.from.position() >= e.to.position())
            continue; // insertions are painted as a marker
        QTextEdit::ExtraSelection sel;
        if (i == next) {
            sel.format.setBackground(t.diffDelBg);
            sel.format.setFontStrikeOut(true);
        } else {
            sel.format.setUnderlineStyle(QTextCharFormat::DotLine);
            sel.format.setUnderlineColor(t.accent);
        }
        sel.cursor = QTextCursor(document());
        sel.cursor.setPosition(e.from.position());
        sel.cursor.setPosition(e.to.position(), QTextCursor::KeepAnchor);
        extra.append(sel);
    }
}

void CodeEditor::paintPredictions()
{
    m_predictPill = QRect();
    if (m_predictions.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const QRect view = viewport()->rect();
    const int next = nextPrediction();

    // Insertion points: a thin bar where the argument goes.
    for (int i = 0; i < m_predictions.size(); ++i) {
        const PendingEdit &e = m_predictions.at(i);
        if (e.from.position() != e.to.position() || !e.from.block().isVisible())
            continue;
        const QRect r = cursorRect(e.from);
        if (!view.intersects(r))
            continue;
        QColor bar = t.gitAdded;
        bar.setAlpha(i == next ? 255 : 150);
        p.fillRect(QRect(r.left() - 1, r.top() + 1, i == next ? 3 : 2, r.height() - 2), bar);
    }

    const PendingEdit &e = m_predictions.at(next);
    QString what;
    switch (EditPredict::Kind(m_predictKind)) {
    case EditPredict::Kind::Rename: what = m_predictFrom + QStringLiteral("  →  ") + e.text; break;
    case EditPredict::Kind::AddArgument: what = tr("add %1 to %2()").arg(m_predictTo, m_predictFrom); break;
    case EditPredict::Kind::RemoveArgument: what = tr("remove %1 from %2()").arg(m_predictTo, m_predictFrom); break;
    case EditPredict::Kind::None: return;
    }
    const int done = m_predictTotal - m_predictions.size();
    const QString count = m_predictTotal > 1 ? QStringLiteral("%1/%2").arg(done + 1).arg(m_predictTotal) : QString();

    const QRect target = cursorRect(e.from);
    const bool onScreen = e.from.block().isVisible() && view.contains(target.center());
    if (!onScreen) {
        const bool below = e.from.position() > cursorForPosition(QPoint(0, 0)).position();
        what = (below ? QStringLiteral("↓  ") : QStringLiteral("↑  ")) +
               tr("line %1").arg(e.from.blockNumber() + 1) + QStringLiteral("  ·  ") + what;
    }

    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    QFont keyFont = f;
    keyFont.setBold(true);
    QFont capFont = f;
    capFont.setPointSizeF(f.pointSizeF() * 0.85);
    const QFontMetrics fm(f), km(keyFont), cm(capFont);
    const QString key = tr("Tab");
    const QString cap = m_predictions.size() > 1 ? tr("Ctrl+Shift+Enter") : QString();
    const QString capLabel = tr("all");
    const int h = fm.height() + 8;
    const int chipH = h - 2 * kPillPad;
    const int iconSz = qMax(10, km.height() - 3);
    const int keyW = 9 + iconSz + 5 + km.horizontalAdvance(key) + 10;
    const int textW = fm.horizontalAdvance(what);
    const int countW = count.isEmpty() ? 0 : 2 * kPillGap + 1 + fm.horizontalAdvance(count);
    const int capW = cap.isEmpty() ? 0 : cm.horizontalAdvance(cap) + 12;
    const int hintW = cap.isEmpty() ? 0 : 2 * kPillGap + 1 + capW + 6 + fm.horizontalAdvance(capLabel);
    const int w = kPillPad + keyW + 12 + textW + countW + hintW + 14;

    QRect pill;
    if (onScreen) {
        QTextCursor end(e.from.block());
        end.movePosition(QTextCursor::EndOfBlock);
        const QRect lineEnd = cursorRect(end);
        int x = lineEnd.right() + fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 3;
        int y = target.top() + (target.height() - h) / 2;
        if (x + w > view.width() - 8) { // no room after the line: just below the place
            x = qBound(4, target.left(), view.width() - w - 8);
            y = target.bottom() + 4;
        }
        pill = QRect(x, y, w, h);
    } else {
        const int x = (view.width() - w) / 2;
        const bool below = e.from.position() > cursorForPosition(QPoint(0, 0)).position();
        pill = QRect(x, below ? view.height() - h - 12 : 12, w, h);
    }

    // A soft shadow, then the body: the panel colour washed with a little accent, and a faint accent outline.
    const QRectF body = QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = h / 2.0;
    p.setPen(Qt::NoPen);
    for (int i = 3; i >= 1; --i) {
        QColor sh(0, 0, 0, t.dark ? 34 : 14);
        p.setBrush(sh);
        p.drawRoundedRect(body.adjusted(-i, -i + 2, i, i + 2), radius + i, radius + i);
    }
    QColor wash = t.accent;
    wash.setAlpha(t.dark ? 26 : 16);
    p.setBrush(t.panel);
    p.drawRoundedRect(body, radius, radius);
    p.setBrush(wash);
    QColor outline = t.accent;
    outline.setAlpha(150);
    p.setPen(QPen(outline, 1));
    p.drawRoundedRect(body, radius, radius);

    // The Tab chip: the tab icon and the key name on the accent.
    const QRect keyRect(pill.left() + kPillPad, pill.top() + kPillPad, keyW, chipH);
    p.setPen(Qt::NoPen);
    p.setBrush(t.accent);
    p.drawRoundedRect(keyRect, chipH / 2.0, chipH / 2.0);
    const qreal dpr = viewport()->devicePixelRatioF();
    const QPixmap icon = Icons::pixmap(QStringLiteral(":/new-icons/tab.svg"), t.onAccent(), iconSz, dpr);
    p.drawPixmap(QRect(keyRect.left() + 9, keyRect.center().y() - iconSz / 2 + 1, iconSz, iconSz), icon);
    p.setFont(keyFont);
    p.setPen(t.onAccent());
    p.drawText(QRect(keyRect.left() + 9 + iconSz + 5, keyRect.top(), keyW - iconSz - 14, chipH),
               Qt::AlignVCenter | Qt::AlignLeft, key);

    QColor divider = t.textMuted;
    divider.setAlpha(90);
    const auto drawDivider = [&](int at) {
        p.setPen(QPen(divider, 1));
        p.drawLine(QPointF(at + 0.5, pill.top() + h * 0.28), QPointF(at + 0.5, pill.bottom() - h * 0.28));
    };

    p.setFont(f);
    p.setPen(t.editorFg);
    int x = keyRect.right() + 1 + 12;
    p.drawText(QRect(x, pill.top(), textW, h), Qt::AlignVCenter | Qt::AlignLeft, what);
    x += textW;
    if (countW) {
        drawDivider(x + kPillGap);
        p.setFont(f);
        p.setPen(t.textMuted);
        p.drawText(QRect(x + 2 * kPillGap + 1, pill.top(), countW - 2 * kPillGap - 1, h), Qt::AlignVCenter | Qt::AlignLeft,
                   count);
        x += countW;
    }
    if (hintW) {
        drawDivider(x + kPillGap);
        x += 2 * kPillGap + 1;
        // The bulk shortcut as a small outlined keycap, then what it does.
        const int capH = cm.height() + 2;
        const QRectF capRect(x + 0.5, pill.top() + (h - capH) / 2.0 + 0.5, capW - 1, capH - 1);
        QColor capBorder = t.textMuted;
        capBorder.setAlpha(120);
        p.setPen(QPen(capBorder, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(capRect, 4, 4);
        p.setFont(capFont);
        p.setPen(t.textMuted);
        p.drawText(capRect, Qt::AlignCenter, cap);
        x += capW + 6;
        p.setFont(f);
        p.drawText(QRect(x, pill.top(), hintW, h), Qt::AlignVCenter | Qt::AlignLeft, capLabel);
    }
    m_predictPill = pill;
}
