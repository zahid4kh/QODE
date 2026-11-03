#include "ContextMeter.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QEvent>
#include <QPainter>

namespace claudecode {

namespace {
QString tokens(int n)
{
    if (n >= 1000000)
        return QStringLiteral("%1M").arg(n / 1000000.0, 0, 'f', n % 1000000 ? 1 : 0);
    return n >= 1000 ? QStringLiteral("%1k").arg(qRound(n / 1000.0)) : QString::number(n);
}
} // namespace

ContextMeter::ContextMeter(QWidget *parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("ccContextMeter"));
    setCursor(Qt::PointingHandCursor);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, qOverload<>(&QWidget::update));
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, [this] { // the zoom may have changed
        updateGeometry();
        update();
    });
    updateToolTip();
}

void ContextMeter::setUsage(int used, int window, bool compacted)
{
    if (used == m_used && window == m_window && compacted == m_compacted)
        return;
    m_used = used;
    m_window = window;
    m_compacted = compacted;
    updateToolTip();
    update();
}

QSize ContextMeter::sizeHint() const
{
    const int s = SettingsManager::instance().scaled(16) + 8;
    return {s, s};
}

void ContextMeter::updateToolTip()
{
    QString text;
    if (m_used < 0 || m_window <= 0)
        text = m_compacted ? tr("Compacted: size known after the next reply") : tr("Context: size known after the first reply");
    else
        text = tr("%1 of %2 tokens (%3 %)").arg(tokens(m_used), tokens(m_window)).arg(qRound(100.0 * m_used / m_window));
    setToolTip(isEnabled() ? text + tr(" · Click to compact the conversation") : text);
}

void ContextMeter::changeEvent(QEvent *event)
{
    QToolButton::changeEvent(event);
    if (event->type() == QEvent::EnabledChange)
        updateToolTip();
}

void ContextMeter::paintEvent(QPaintEvent *)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (underMouse() && isEnabled()) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.border);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }
    const qreal side = SettingsManager::instance().scaled(16);
    const qreal pen = qMax<qreal>(2.0, side / 7.0);
    const QRectF ring(QPointF((width() - side) / 2 + pen / 2, (height() - side) / 2 + pen / 2), QSizeF(side - pen, side - pen));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(t.border, pen, Qt::SolidLine, Qt::FlatCap));
    p.drawEllipse(ring);
    if (m_used < 0 || m_window <= 0)
        return;
    const qreal share = qBound(0.0, qreal(m_used) / m_window, 1.0);
    const QColor fill = share >= 0.9 ? t.danger : share >= 0.7 ? t.warning : t.accent;
    p.setPen(QPen(isEnabled() ? fill : t.textMuted, pen, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(ring, 90 * 16, -qRound(share * 360 * 16)); // clockwise from twelve o'clock
}

} // namespace claudecode
