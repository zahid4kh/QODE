#include "game/GamePanel.h"

#include "game/GamePicker.h"
#include "game/GameStore.h"
#include "game/SnakeWidget.h"
#include "game/TeqrisWidget.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
const QString ArcadeId = QStringLiteral("arcade"); // the panel's own GameStore: which game was played last
const QString LastGameKey = QStringLiteral("lastGame");
} // namespace

GamePanel::GamePanel(QWidget *parent)
    : QWidget(parent)
{
    m_backBtn = new QToolButton(this);
    Icons::bind(m_backBtn, QStringLiteral(":/new-icons/arrow-left.svg"));
    m_backBtn->setToolTip(tr("All games (Escape on a game's menu); a running game pauses"));
    m_backBtn->setAutoRaise(true);
    m_backBtn->setFocusPolicy(Qt::NoFocus); // clicking it must not take the keys away from the game first
    connect(m_backBtn, &QToolButton::clicked, this, &GamePanel::showPicker);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);

    auto *closeBtn = new QToolButton(this);
    Icons::bind(closeBtn, QStringLiteral(":/new-icons/x.svg"));
    closeBtn->setToolTip(tr("Close the game panel (Ctrl+Alt+P); a running game pauses"));
    closeBtn->setAutoRaise(true);
    closeBtn->setFocusPolicy(Qt::NoFocus);
    connect(closeBtn, &QToolButton::clicked, this, &GamePanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->setSpacing(2);
    hl->addWidget(m_backBtn);
    hl->addWidget(m_title, 1);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_picker = new GamePicker(this);
    m_picker->select(GameStore(ArcadeId).option(LastGameKey, QStringLiteral("teqris")).toString());
    connect(m_picker, &GamePicker::gameChosen, this, &GamePanel::showGame);
    connect(m_picker, &GamePicker::closeRequested, this, &GamePanel::closeRequested);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_picker);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_stack, 1);

    setPage(m_picker, tr("GAMES"));
}

int GamePanel::preferredWidth()
{
    return SettingsManager::instance().scaled(560);
}

void GamePanel::showPicker()
{
    setPage(m_picker, tr("GAMES"));
}

void GamePanel::showGame(const QString &id)
{
    QWidget *page = nullptr;
    if (id == QLatin1String("snake")) {
        if (!m_snake) {
            m_snake = new SnakeWidget(this);
            connect(m_snake, &SnakeWidget::exitRequested, this, &GamePanel::showPicker);
            m_stack->addWidget(m_snake);
        }
        page = m_snake;
    } else {
        if (!m_teqris) {
            m_teqris = new TeqrisWidget(this);
            connect(m_teqris, &TeqrisWidget::exitRequested, this, &GamePanel::showPicker);
            m_stack->addWidget(m_teqris);
        }
        page = m_teqris;
    }
    GameStore(ArcadeId).setOption(LastGameKey, id);
    m_picker->select(id);
    setPage(page, GamePicker::gameName(id));
}

// Shows a page of the stack; the panel's focus goes with it, so the keys reach the game or the picker.
void GamePanel::setPage(QWidget *page, const QString &title)
{
    m_stack->setCurrentWidget(page);
    m_title->setText(title);
    m_backBtn->setVisible(page != m_picker);
    setFocusProxy(page);
    if (isVisible())
        page->setFocus();
}
