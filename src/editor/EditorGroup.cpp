#include "EditorGroup.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

// Reorders tabs like a normal movable QTabBar, but dragging a tab well away from the bar hands over to a
// real drag so it can be dropped into another group.
class GroupTabBar : public QTabBar
{
    Q_OBJECT
public:
    using QTabBar::QTabBar;

signals:
    void dragOut(int index);
    void contextRequested(int index, const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        m_pressed = e->button() == Qt::LeftButton ? tabAt(e->position().toPoint()) : -1;
        QTabBar::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressed >= 0 && (e->buttons() & Qt::LeftButton)) {
            const QPoint p = e->position().toPoint();
            if (!rect().adjusted(-40, -12, 40, 12).contains(p)) {
                // End the bar's own reorder, then start the drag.
                QMouseEvent release(QEvent::MouseButtonRelease, e->position(), e->globalPosition(), Qt::LeftButton, Qt::NoButton, e->modifiers());
                QTabBar::mouseReleaseEvent(&release);
                const int index = m_pressed;
                m_pressed = -1;
                emit dragOut(index);
                return;
            }
        }
        QTabBar::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        m_pressed = -1;
        QTabBar::mouseReleaseEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        const int i = tabAt(e->pos());
        if (i >= 0)
            emit contextRequested(i, e->globalPos());
    }

private:
    int m_pressed = -1;
};

class GroupTabs : public QTabWidget
{
public:
    explicit GroupTabs(GroupTabBar *bar, QWidget *parent)
        : QTabWidget(parent)
    {
        setTabBar(bar);
    }
};
}

class EditorGroup::Overlay : public QWidget
{
public:
    explicit Overlay(EditorGroup *group)
        : QWidget(group)
        , m_group(group)
    {
        setAcceptDrops(true);
        hide();
    }

protected:
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (!e->mimeData()->hasFormat(EditorGroup::mimeType()))
            return;
        e->acceptProposedAction();
        m_zone = zoneAt(e->position().toPoint());
        update();
    }
    void dragMoveEvent(QDragMoveEvent *e) override
    {
        const Zone z = zoneAt(e->position().toPoint());
        if (z != m_zone) {
            m_zone = z;
            update();
        }
        e->acceptProposedAction();
    }
    void dropEvent(QDropEvent *e) override
    {
        const Zone z = zoneAt(e->position().toPoint());
        e->acceptProposedAction();
        emit m_group->tabDropped(z);
    }
    void paintEvent(QPaintEvent *) override
    {
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        QColor fill = t.accent;
        fill.setAlpha(60);
        QColor edge = t.accent;
        edge.setAlpha(200);
        QPainter p(this);
        p.setPen(QPen(edge, 2));
        p.setBrush(fill);
        p.drawRect(zoneRect(m_zone).adjusted(1, 1, -1, -1));
    }

private:
    Zone zoneAt(const QPoint &p) const
    {
        const qreal fx = qreal(p.x()) / qMax(1, width());
        const qreal fy = qreal(p.y()) / qMax(1, height());
        if (p.y() < 34) // the strip with the tabs always means "into this group"
            return Center;
        if (fx < 0.28)
            return Left;
        if (fx > 0.72)
            return Right;
        if (fy < 0.3)
            return Top;
        if (fy > 0.7)
            return Bottom;
        return Center;
    }
    QRect zoneRect(Zone z) const
    {
        const int w = width(), h = height();
        switch (z) {
        case Left: return QRect(0, 0, w / 2, h);
        case Right: return QRect(w - w / 2, 0, w / 2, h);
        case Top: return QRect(0, 0, w, h / 2);
        case Bottom: return QRect(0, h - h / 2, w, h / 2);
        default: return rect();
        }
    }

    EditorGroup *m_group;
    Zone m_zone = Center;
};

EditorGroup::EditorGroup(QWidget *parent)
    : QWidget(parent)
{
    auto *bar = new GroupTabBar;
    m_tabs = new GroupTabs(bar, this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    bar->setMovable(true);
    bar->setExpanding(false);
    bar->setElideMode(Qt::ElideRight);
    bar->setUsesScrollButtons(true);
    connect(bar, &GroupTabBar::dragOut, this, &EditorGroup::tabDragStarted);
    connect(bar, &GroupTabBar::contextRequested, this, &EditorGroup::tabContextMenuRequested);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_tabs);

    m_overlay = new Overlay(this);
}

void EditorGroup::setDragActive(bool active)
{
    m_overlay->setGeometry(rect());
    m_overlay->setVisible(active);
    if (active)
        m_overlay->raise();
}

QString EditorGroup::mimeType()
{
    return QStringLiteral("application/x-qode-tab");
}

#include "EditorGroup.moc"
