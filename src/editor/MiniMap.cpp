#include "MiniMap.h"

#include "CodeEditor.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

MiniMap::MiniMap(CodeEditor *editor)
    : QWidget(editor)
    , m_editor(editor)
{
    setFixedWidth(kWidth);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    connect(editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &MiniMap::scheduleUpdate);
    connect(editor->verticalScrollBar(), &QScrollBar::rangeChanged, this, &MiniMap::scheduleUpdate);
}

void MiniMap::invalidate()
{
    m_dirty = true;
    scheduleUpdate();
}

void MiniMap::scheduleUpdate()
{
    if (isVisible() && !m_timer->isActive())
        m_timer->start();
}

void MiniMap::rebuildRows()
{
    m_rows.clear();
    m_rows.reserve(m_editor->blockCount());
    for (QTextBlock b = m_editor->document()->begin(); b.isValid(); b = b.next())
        if (b.isVisible())
            m_rows.append(b.blockNumber());
    m_dirty = false;
}

int MiniMap::rowForBlock(int blockNumber) const
{
    return int(std::lower_bound(m_rows.cbegin(), m_rows.cend(), blockNumber) - m_rows.cbegin());
}

int MiniMap::firstShownRow() const
{
    const int total = m_rows.size();
    const int capacity = qMax(1, height() / kRowHeight);
    if (total <= capacity)
        return 0;
    const QScrollBar *sb = m_editor->verticalScrollBar();
    const double ratio = sb->maximum() > 0 ? double(sb->value()) / sb->maximum() : 0.0;
    return int(std::lround(ratio * (total - capacity)));
}

void MiniMap::paintEvent(QPaintEvent *)
{
    if (m_dirty)
        rebuildRows();
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPainter p(this);
    p.fillRect(rect(), t.editorBg);
    p.setPen(t.border);
    p.drawLine(0, 0, 0, height());

    const int total = m_rows.size();
    if (total == 0)
        return;
    const int capacity = height() / kRowHeight + 1;
    const int start = firstShownRow();
    const int end = qMin(total, start + capacity);
    const int tab = qMax(1, SettingsManager::instance().tabSize());
    const int maxCol = kWidth - 12;
    const QTextDocument *doc = m_editor->document();

    QColor plain = t.editorFg;
    plain.setAlpha(t.dark ? 120 : 130);

    // Viewport band first so the text stays on top of it.
    int firstVisible = 0, lastVisible = 0;
    m_editor->visibleBlockRange(&firstVisible, &lastVisible);
    const int r0 = rowForBlock(firstVisible), r1 = qMax(r0, rowForBlock(lastVisible));
    QColor band = t.accent;
    band.setAlpha(t.dark ? 34 : 28);
    const QRect bandRect(1, (r0 - start) * kRowHeight, width() - 1, (r1 - r0 + 1) * kRowHeight);
    p.fillRect(bandRect, band);

    for (int row = start; row < end; ++row) {
        const QTextBlock b = doc->findBlockByNumber(m_rows.at(row));
        if (!b.isValid())
            continue;
        const int y = (row - start) * kRowHeight;
        const QString text = b.text();
        const auto formats = b.layout() ? b.layout()->formats() : QList<QTextLayout::FormatRange>();
        int fi = 0;
        int col = 0;
        int runStart = -1, runLen = 0;
        QColor runColor;
        auto flush = [&] {
            if (runLen > 0)
                p.fillRect(4 + runStart, y, runLen, kRowHeight - 1, runColor);
            runLen = 0;
        };
        for (int i = 0; i < text.size() && col < maxCol; ++i) {
            const QChar ch = text.at(i);
            if (ch == QLatin1Char('\t')) {
                flush();
                col += tab - col % tab;
                continue;
            }
            if (ch.isSpace()) {
                flush();
                ++col;
                continue;
            }
            while (fi < formats.size() && formats.at(fi).start + formats.at(fi).length <= i)
                ++fi;
            QColor c = plain;
            if (fi < formats.size() && formats.at(fi).start <= i && formats.at(fi).format.foreground().style() != Qt::NoBrush) {
                c = formats.at(fi).format.foreground().color();
                c.setAlpha(t.dark ? 210 : 200);
            }
            if (runLen > 0 && runColor == c && runStart + runLen == col) {
                ++runLen;
            } else {
                flush();
                runStart = col;
                runLen = 1;
                runColor = c;
            }
            ++col;
        }
        flush();
    }

    // Git change marks along the right edge.
    for (const GitDiff::Hunk &h : m_editor->hunks()) {
        const int first = h.isDeleted() ? h.newStart : h.newStart;
        const int last = h.isDeleted() ? h.newStart : h.newStart + h.newCount - 1;
        const int ra = rowForBlock(first), rb = qMax(ra, rowForBlock(last));
        if (rb < start || ra >= end)
            continue;
        const QColor c = h.isAdded() ? t.gitAdded : (h.isDeleted() ? t.gitDeleted : t.accent);
        p.fillRect(width() - 3, (ra - start) * kRowHeight, 3, qMax(2, (rb - ra + 1) * kRowHeight), c);
    }

    // Edges of the viewport band.
    QColor edge = t.accent;
    edge.setAlpha(120);
    p.setPen(edge);
    p.drawLine(1, bandRect.top(), width(), bandRect.top());
    p.drawLine(1, bandRect.bottom(), width(), bandRect.bottom());
}

// Scrolls so the line under widget-y `y` ends up in the middle of the editor.
void MiniMap::scrollToY(int y)
{
    if (m_dirty)
        rebuildRows();
    if (m_rows.isEmpty())
        return;
    const int row = qBound(0, firstShownRow() + y / kRowHeight, int(m_rows.size()) - 1);
    m_editor->scrollBlockToCenter(m_rows.at(row));
}

void MiniMap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    scrollToY(int(event->position().y()));
}

void MiniMap::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging)
        scrollToY(int(event->position().y()));
}

void MiniMap::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
}

void MiniMap::wheelEvent(QWheelEvent *event)
{
    QCoreApplication::sendEvent(m_editor->verticalScrollBar(), event);
}
