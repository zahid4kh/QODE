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
    m_overlay = new Overlay(this);
    m_overlay->raise();
}

void Island::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_overlay->setGeometry(rect());
    m_overlay->raise();
}

void Island::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_overlay->raise();
}
