#include "Island.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace {
constexpr qreal kRadius = 10;
}

class Island::Overlay : public QWidget
{
public:
    explicit Overlay(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect());
        QPainterPath outer;
        outer.addRect(r);
        QPainterPath inner;
        inner.addRoundedRect(r, kRadius, kRadius);
        p.fillPath(outer.subtracted(inner), t.frame);
        QColor edge = t.border;
        edge.setAlphaF(t.dark ? 0.9 : 0.7);
        p.setPen(QPen(edge, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), kRadius - 0.5, kRadius - 0.5);
    }
};

Island::Island(QWidget *content, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("island"));
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
    m_content = content;
    m_overlay = new Overlay(this);
    m_overlay->raise();
}

void Island::setSliding(bool on, int fullWidth)
{
    m_sliding = on;
    m_fullWidth = fullWidth;
    layout()->setEnabled(!on);
    if (on) {
        m_content->setGeometry(qMin(0, width() - m_fullWidth), 0, qMax(width(), m_fullWidth), height());
    } else {
        layout()->setEnabled(true);
        layout()->activate();
    }
    updateGeometry();
}

QSize Island::minimumSizeHint() const
{
    return m_sliding ? QSize(0, 0) : QWidget::minimumSizeHint();
}

void Island::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_sliding)
        m_content->setGeometry(qMin(0, width() - m_fullWidth), 0, qMax(width(), m_fullWidth), height());
    m_overlay->setGeometry(rect());
    m_overlay->raise();
}

void Island::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_overlay->raise();
}
