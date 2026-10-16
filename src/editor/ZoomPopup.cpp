#include "ZoomPopup.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

void ZoomPopup::open(QWidget *anchor)
{
    auto *p = new ZoomPopup(anchor);
    p->adjustSize();
    const QPoint top = anchor->mapToGlobal(QPoint(anchor->width() / 2, 0));
    QPoint pos(top.x() - p->width() / 2, top.y() - p->height() - 6);
    const QRect avail = anchor->screen()->availableGeometry();
    pos.setX(qBound(avail.left() + 4, pos.x(), avail.right() - p->width() - 4));
    pos.setY(qMax(avail.top() + 4, pos.y()));
    p->move(pos);
    p->show();
}

ZoomPopup::ZoomPopup(QWidget *anchor)
    : QFrame(anchor->window(), Qt::Popup)
{
    setAttribute(Qt::WA_DeleteOnClose);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setObjectName(QStringLiteral("zoomPopup"));
    setStyleSheet(QStringLiteral("#zoomPopup { background: %1; border: 1px solid %2; border-radius: 8px; }"
                                 "#zoomPopup QLabel { background: transparent; color: %3; }"
                                 "#zoomPopup QToolButton { background: %4; border: 1px solid %2; border-radius: 6px; }"
                                 "#zoomPopup QToolButton:hover:enabled { background: %5; }"
                                 "#zoomPopup QPushButton { background: transparent; color: %3; border: 1px solid %2; border-radius: 6px; padding: 3px 12px; }"
                                 "#zoomPopup QPushButton:hover:enabled { background: %5; }"
                                 "#zoomPopup QSlider::groove:horizontal { height: 4px; background: %2; border-radius: 2px; }"
                                 "#zoomPopup QSlider::sub-page:horizontal { background: %6; border-radius: 2px; }"
                                 "#zoomPopup QSlider::handle:horizontal { background: %6; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }")
                      .arg(t.panel.name(), t.border.name(), t.editorFg.name(), t.editorBg.name(), t.currentLine.name(), t.accent.name()));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    auto *title = new QLabel(tr("Zoom"), this);
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    root->addWidget(title);

    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    m_minus = new QToolButton(this);
    m_minus->setIcon(Icons::tinted(QStringLiteral(":/new-icons/zoom-out.svg"), t.editorFg));
    m_minus->setToolTip(tr("Zoom Out (Ctrl+-)"));
    m_plus = new QToolButton(this);
    m_plus->setIcon(Icons::tinted(QStringLiteral(":/new-icons/zoom-in.svg"), t.editorFg));
    m_plus->setToolTip(tr("Zoom In (Ctrl+=)"));
    for (QToolButton *b : {m_minus, m_plus}) {
        b->setFixedSize(34, 30);
        b->setIconSize(QSize(16, 16));
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoRepeat(true);
    }
    m_percent = new QLabel(this);
    m_percent->setAlignment(Qt::AlignCenter);
    m_percent->setMinimumWidth(64);
    QFont pf = m_percent->font();
    pf.setPointSizeF(pf.pointSizeF() * 1.4);
    pf.setBold(true);
    m_percent->setFont(pf);
    row->addWidget(m_minus);
    row->addWidget(m_percent, 1);
    row->addWidget(m_plus);
    root->addLayout(row);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(5, 40); // x10 percent
    m_slider->setPageStep(1);
    root->addWidget(m_slider);

    auto *foot = new QHBoxLayout;
    m_size = new QLabel(this);
    QPalette pal = m_size->palette();
    pal.setColor(QPalette::WindowText, t.textMuted);
    m_size->setPalette(pal);
    m_size->setStyleSheet(QStringLiteral("color: %1;").arg(t.textMuted.name()));
    m_reset = new QPushButton(tr("Reset"), this);
    m_reset->setToolTip(tr("Back to 100% (Ctrl+0)"));
    m_reset->setCursor(Qt::PointingHandCursor);
    foot->addWidget(m_size, 1);
    foot->addWidget(m_reset);
    root->addLayout(foot);

    connect(m_minus, &QToolButton::clicked, this, [this] { step(-10); });
    connect(m_plus, &QToolButton::clicked, this, [this] { step(10); });
    connect(m_reset, &QPushButton::clicked, this, [] { SettingsManager::instance().setUiZoom(100); });
    connect(m_slider, &QSlider::valueChanged, this, [](int v) { SettingsManager::instance().setUiZoom(v * 10); });
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &ZoomPopup::refresh);
    refresh();
}

void ZoomPopup::step(int delta)
{
    auto &s = SettingsManager::instance();
    s.setUiZoom(s.uiZoom() + delta);
}

void ZoomPopup::refresh()
{
    auto &s = SettingsManager::instance();
    const int z = s.uiZoom();
    m_percent->setText(QStringLiteral("%1%").arg(z));
    const int base = s.baseEditorFontSize();
    m_size->setText(tr("Whole interface · editor text %1 pt (base %2 pt)").arg(QString::number(base * z / 100.0, 'f', 1), QString::number(base)));
    m_minus->setEnabled(z > 50);
    m_plus->setEnabled(z < 400);
    m_reset->setEnabled(z != 100);
    QSignalBlocker b(m_slider);
    m_slider->setValue(qRound(z / 10.0));
}
