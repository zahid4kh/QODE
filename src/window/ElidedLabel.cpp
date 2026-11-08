#include "window/ElidedLabel.h"

#include <QEvent>
#include <QFontMetrics>

ElidedLabel::ElidedLabel(QWidget *parent, Qt::TextElideMode mode)
    : QLabel(parent), m_mode(mode)
{
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::setFullText(const QString &text)
{
    m_full = text;
    setToolTip(text);
    refit();
}

QSize ElidedLabel::minimumSizeHint() const
{
    return {0, QLabel::minimumSizeHint().height()};
}

QSize ElidedLabel::sizeHint() const
{
    return {0, QLabel::sizeHint().height()};
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    refit();
}

void ElidedLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        refit();
}

void ElidedLabel::refit()
{
    const QMargins m = contentsMargins();
    const int room = qMax(0, width() - m.left() - m.right() - 2 * margin());
    const QString shown = fontMetrics().elidedText(m_full, m_mode, room);
    if (shown != text())
        setText(shown);
}
