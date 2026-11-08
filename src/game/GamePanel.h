#pragma once

#include <QWidget>

class GamePicker;
class QLabel;
class QStackedWidget;
class QToolButton;
class SnakeWidget;
class TeqrisWidget;

// The game panel in the right-hand island: something to play while the QODE chat or Claude Code works. A header
// (back to the picker, title, close) over a stack: the picker (GamePicker) and the games, each created on first use.
// Every game keeps its scores in ~/.config/QODE-AGENTIC/games/<id>.json (GameStore); the last game played is
// remembered in games/arcade.json, so the picker opens on its card. Leaving a game (back, close, focus) pauses it.
class GamePanel : public QWidget
{
    Q_OBJECT
public:
    explicit GamePanel(QWidget *parent = nullptr);

    // Minimum width at which the games are comfortable to play (at the current zoom).
    static int preferredWidth();

signals:
    void closeRequested();

private:
    void showPicker();
    void showGame(const QString &id);
    void setPage(QWidget *page, const QString &title);

    QLabel *m_title;
    QToolButton *m_backBtn;
    QStackedWidget *m_stack;
    GamePicker *m_picker;
    TeqrisWidget *m_teqris = nullptr;
    SnakeWidget *m_snake = nullptr;
};
