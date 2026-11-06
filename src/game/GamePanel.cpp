#include "game/GamePanel.h"

#include "game/TeqrisWidget.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

GamePanel::GamePanel(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(tr("TEQRIS"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);

    auto *closeBtn = new QToolButton(this);
    Icons::bind(closeBtn, QStringLiteral(":/new-icons/x.svg"));
    closeBtn->setToolTip(tr("Close the game panel (Ctrl+Alt+P); a running game pauses"));
    closeBtn->setAutoRaise(true);
    closeBtn->setFocusPolicy(Qt::NoFocus); // clicking it must not take the keys away from the game first
    connect(closeBtn, &QToolButton::clicked, this, &GamePanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(m_title, 1);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_teqris = new TeqrisWidget(this);
    connect(m_teqris, &TeqrisWidget::closeRequested, this, &GamePanel::closeRequested);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_teqris, 1);

    setFocusProxy(m_teqris);
}

int GamePanel::preferredWidth()
{
    return SettingsManager::instance().scaled(560);
}
