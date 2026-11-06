#pragma once

#include <QWidget>

class QLabel;
class TeqrisWidget;

// The game panel in the right-hand island: something to play while the QODE chat or Claude Code works. A header
// (title, close) over the game, which keeps its scores in ~/.config/QODE-AGENTIC/games/<id>.json (GameStore).
// Today it holds Teqris; another game would go beside it in a stack with a picker in the header.
class GamePanel : public QWidget
{
    Q_OBJECT
public:
    explicit GamePanel(QWidget *parent = nullptr);

    // Minimum width at which the game is comfortable to play (at the current zoom).
    static int preferredWidth();

signals:
    void closeRequested();

private:
    QLabel *m_title;
    TeqrisWidget *m_teqris;
};
