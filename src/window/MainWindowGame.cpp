// The game panel in the right-hand island (Ctrl+Alt+P): something to play while the QODE chat or Claude Code works.
#include "MainWindow.h"

#include "app/StartupProfile.h"
#include "game/GamePanel.h"

#include <QSplitter>
#include <QStackedWidget>

// Created the first time it is shown.
GamePanel *MainWindow::gamePanel()
{
    if (m_gamePanel)
        return m_gamePanel;
    StartupProfile::Later timing(QStringLiteral("game panel"));
    m_gamePanel = new GamePanel(this);
    m_rightStack->addWidget(m_gamePanel);
    connect(m_gamePanel, &GamePanel::closeRequested, this, &MainWindow::hideMedia);
    return m_gamePanel;
}

void MainWindow::toggleGamePanel()
{
    if (rightPanelShows(m_gamePanel)) {
        hideMedia();
        return;
    }
    showInRightPanel(gamePanel());
    // The game is drawn to fit, but in a narrow island the well gets tiny: widen it, up to half the window.
    QWidget *island = m_rightStack->parentWidget();
    const int want = qMin(GamePanel::preferredWidth(), m_hsplit->width() / 2);
    if (island->width() < want) {
        QList<int> sizes = m_hsplit->sizes();
        const int grow = want - sizes[2];
        sizes[2] = want;
        sizes[1] = qMax(200, sizes[1] - grow);
        m_hsplit->setSizes(sizes);
    }
    m_gamePanel->setFocus();
}
