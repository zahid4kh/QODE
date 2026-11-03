// Code lenses: action labels after a line's text ("▶ Run | Debug", "3 references"), clickable.
#include "CodeEditor.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QMap>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTimer>

QString CodeEditor::codeLensLabel(const QString &title)
{
    // VS Code's $(icon) markers: the run icon becomes a glyph, the others go (the word next to them says enough).
    static const QRegularExpression icon(QStringLiteral(R"(\$\(([\w-]+)(?:~[\w-]+)?\))"));
    QString out;
    int from = 0;
    for (QRegularExpressionMatchIterator it = icon.globalMatch(title); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += title.mid(from, m.capturedStart() - from);
        const QString name = m.captured(1);
        if (name == QLatin1String("play") || name == QLatin1String("run") || name == QLatin1String("run-all"))
            out += QStringLiteral("▶");
        from = int(m.capturedEnd());
    }
    out += title.mid(from);
    return out.simplified();
}

void CodeEditor::setCodeLenses(const QVector<LspCodeLens> &lenses)
{
    // What each spot showed, so a lens that still has to be resolved keeps its old label until the answer comes.
    QHash<QPair<int, int>, QStringList> before;
    for (const Lens &l : std::as_const(m_lenses))
        if (!l.at.isNull() && !l.shown.isEmpty())
            before[{l.at.blockNumber(), l.at.positionInBlock()}].append(l.shown);
    QHash<QPair<int, int>, int> taken;
    ++m_lensGeneration;
    m_lenses.clear();
    m_lensHover = -1;
    for (const LspCodeLens &lens : lenses) {
        const QTextBlock b = document()->findBlockByNumber(lens.line);
        if (!b.isValid())
            continue;
        Lens l;
        l.at = QTextCursor(document());
        l.at.setPosition(b.position() + qBound(0, lens.column, qMax(0, b.length() - 1)));
        l.lens = lens;
        if (lens.resolved()) {
            l.shown = codeLensLabel(lens.title);
        } else {
            const QPair<int, int> key{lens.line, lens.column};
            const QStringList old = before.value(key);
            int &n = taken[key];
            if (n < old.size())
                l.shown = old.at(n++);
        }
        m_lenses.append(l);
    }
    viewport()->update();
}

void CodeEditor::resolveCodeLens(int generation, int index, const LspCodeLens &lens)
{
    if (generation != m_lensGeneration || index < 0 || index >= m_lenses.size())
        return;
    Lens &l = m_lenses[index];
    l.lens = lens;
    l.shown = lens.resolved() ? codeLensLabel(lens.title) : QString();
    viewport()->update();
}

void CodeEditor::clearCodeLenses()
{
    if (m_lenses.isEmpty())
        return;
    ++m_lensGeneration;
    m_lenses.clear();
    m_lensHover = -1;
    viewport()->update();
}

LspCodeLens CodeEditor::codeLens(int index) const
{
    return index >= 0 && index < m_lenses.size() ? m_lenses.at(index).lens : LspCodeLens();
}

QString CodeEditor::codeLensText(int line) const
{
    QStringList labels;
    for (const Lens &l : m_lenses)
        if (!l.at.isNull() && l.at.blockNumber() == line && !l.shown.isEmpty())
            labels << l.shown;
    return labels.join(QStringLiteral(" | "));
}

int CodeEditor::codeLensAt(const QPoint &pos) const
{
    for (int i = 0; i < m_lenses.size(); ++i) {
        const Lens &l = m_lenses.at(i);
        if (l.rect.contains(pos) && !l.lens.command.isEmpty())
            return i;
    }
    return -1;
}

void CodeEditor::paintCodeLenses()
{
    m_lensEnd.clear();
    for (Lens &l : m_lenses)
        l.rect = QRect();
    if (m_lenses.isEmpty())
        return;
    int first = 0, last = 0;
    visibleBlockRange(&first, &last);
    QMap<int, QVector<int>> rows; // block number -> lenses shown there
    QVector<int> ask;
    for (int i = 0; i < m_lenses.size(); ++i) {
        Lens &l = m_lenses[i];
        if (l.at.isNull())
            continue;
        const QTextBlock b = l.at.block();
        const int bn = b.blockNumber();
        if (bn < first || bn > last || !b.isVisible())
            continue;
        if (!l.lens.resolved() && !l.asked) {
            l.asked = true;
            ask << i;
        }
        if (!l.shown.isEmpty())
            rows[bn] << i;
    }
    if (!ask.isEmpty()) // not from inside the paint: the answer may arrive synchronously and repaint
        QTimer::singleShot(0, this, [this, generation = m_lensGeneration, ask] {
            if (generation != m_lensGeneration)
                return;
            for (int i : ask)
                emit codeLensResolveRequested(generation, i);
        });
    if (rows.isEmpty())
        return;

    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QFont f = hintFont();
    const QFontMetrics fm(f);
    QFont hoverFont = f;
    hoverFont.setUnderline(true);
    QPainter p(viewport());
    const int space = fontMetrics().horizontalAdvance(QLatin1Char(' '));
    const QString sep = QStringLiteral("  |  ");
    QColor sepInk = t.textMuted;
    sepInk.setAlpha(120);
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        QVector<int> order = it.value();
        std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return m_lenses.at(a).lens.column < m_lenses.at(b).lens.column; });
        QTextCursor end(document()->findBlockByNumber(it.key()));
        end.movePosition(QTextCursor::EndOfBlock);
        const QRect line = cursorRect(end);
        int x = line.right() + space * 3;
        for (int k = 0; k < order.size(); ++k) {
            Lens &l = m_lenses[order.at(k)];
            if (k > 0) {
                p.setFont(f);
                p.setPen(sepInk);
                const int w = fm.horizontalAdvance(sep);
                p.drawText(QRect(x, line.top(), w, line.height()), Qt::AlignVCenter | Qt::AlignLeft, sep);
                x += w;
            }
            const bool hover = order.at(k) == m_lensHover;
            const int w = fm.horizontalAdvance(l.shown);
            l.rect = QRect(x, line.top(), w, line.height());
            p.setFont(hover ? hoverFont : f);
            p.setPen(hover ? t.accent : t.textMuted);
            p.drawText(l.rect, Qt::AlignVCenter | Qt::AlignLeft, l.shown);
            x += w;
        }
        m_lensEnd.insert(it.key(), x);
    }
}
