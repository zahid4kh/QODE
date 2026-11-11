#include "window/WrapLabel.h"

#include <QEvent>
#include <QTextDocument>

#include <cmath>

WrapLabel::WrapLabel(QWidget *parent)
    : QLabel(parent)
{
    setWordWrap(true);
}

WrapLabel::WrapLabel(const QString &text, QWidget *parent)
    : QLabel(text, parent)
{
    setWordWrap(true);
}

int WrapLabel::heightForWidth(int width) const
{
    if (width <= 0 || !wordWrap())
        return QLabel::heightForWidth(width);
    if (!m_measured || text() != m_text || font() != m_font)
        measure();
    if (m_fitWidth > 0 && width >= m_fitWidth)
        return m_fitHeight;
    for (const auto &[w, h] : std::as_const(m_cache))
        if (w == width)
            return h;
    const int h = QLabel::heightForWidth(width);
    if (m_cache.size() >= 4)
        m_cache.removeFirst();
    m_cache.append({width, h});
    return h;
}

void WrapLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    // Margins and indent can come from a style sheet; text and font are compared in heightForWidth.
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::ContentsRectChange || event->type() == QEvent::FontChange)
        m_measured = false;
}

// The unwrapped width is estimated from a document set up like the label's text, on the wide side, then checked
// against QLabel itself: only if QLabel gives the unwrapped height at that width is it used. A wrong estimate costs
// the shortcut for this text, never a wrong height.
void WrapLabel::measure() const
{
    m_measured = true;
    m_text = text();
    m_font = font();
    m_fitWidth = 0;
    m_cache.clear();
    m_fitHeight = QLabel::heightForWidth(QWIDGETSIZE_MAX);
    if (m_text.isEmpty() || m_fitHeight <= 0)
        return;
    QTextDocument doc;
    doc.setDefaultFont(m_font);
    const bool rich = textFormat() == Qt::RichText || (textFormat() == Qt::AutoText && Qt::mightBeRichText(m_text));
    if (rich)
        doc.setHtml(m_text);
    else
        doc.setPlainText(m_text);
    const QMargins m = contentsMargins();
    const int extra = m.left() + m.right() + 2 * margin() + qMax(0, indent()) + 2 * int(std::ceil(doc.documentMargin())) + 2;
    const int candidate = int(std::ceil(doc.idealWidth())) + extra;
    if (QLabel::heightForWidth(candidate) == m_fitHeight)
        m_fitWidth = candidate;
}
