#pragma once

#include "game/GameStore.h"
#include "game/SnakeGame.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QTimer>
#include <QTransform>
#include <QVector>
#include <QWidget>

class QPainter;

// Snake, the game panel's second game: draws every screen (menu, controls, game, pause, game over) with QPainter and
// turns keyboard / mouse input into game actions, in an 820x760 logical space scaled to fit, like TeqrisWidget.
// Arrows or WASD steer. It only ticks while shown, pauses when it loses focus or is hidden, and keeps its scores
// and options (speed, walls) in GameStore("snake").
class SnakeWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SnakeWidget(QWidget *parent = nullptr);

    QSize sizeHint() const override;

signals:
    void exitRequested(); // back to the game picker: the menu's ALL GAMES item, or Escape on the menu

protected:
    bool event(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    enum class Screen { Menu, Controls, Countdown, Playing, Paused, Dying, GameOver };

    struct Particle
    {
        QPointF pos, vel;
        QColor color;
        double age = 0, life = 1;
        qreal size = 2;
    };
    struct FloatText
    {
        QString text;
        QPointF pos;
        QColor color;
        qreal size;
        double age = 0, life = 1.1;
    };
    struct Ring
    {
        QPointF pos;
        QColor color;
        double age = 0, life = 0.45;
    };
    struct Star
    {
        qreal x, y, size, speed, phase;
    };

    // flow
    void tick();
    void setScreen(Screen screen);
    void startGame();
    QStringList menuItems() const;
    void activate(const QString &item);
    void changeSpeed(int delta);
    void toggleWalls();
    void handleMenuKey(int key);
    bool steer(int key);
    void processGameEvents();
    void pauseIfRunning();

    // effects
    void updateEffects(double ms);
    void burst(QPointF pos, QColor color, int count, qreal speed);

    // drawing
    void paintBackground(QPainter &p);
    void paintMenu(QPainter &p);
    void paintMenuSnakes(QPainter &p, qreal alpha);
    void paintControls(QPainter &p);
    void paintHud(QPainter &p);
    void paintField(QPainter &p);
    void paintSnake(QPainter &p);
    void paintFood(QPainter &p);
    void paintEffects(QPainter &p);
    void paintCountdown(QPainter &p);
    void paintPause(QPainter &p);
    void paintGameOver(QPainter &p);
    void paintButton(QPainter &p, const QRectF &rect, const QString &text, bool selected);
    QPointF cellCenter(QPointF cell) const;
    QColor accent() const;
    QColor bodyColor(qreal along) const; // 0 = head, 1 = tail

    SnakeGame m_game;
    Screen m_screen = Screen::Menu;
    Screen m_resumeScreen = Screen::Playing;
    int m_selected = 0;
    int m_speed = SnakeGame::Normal;
    bool m_walls = true;
    GameStore m_store;
    int m_best = 0;
    bool m_newBest = false;
    double m_playMs = 0; // time spent in Playing this game, for the store
    QPoint m_crash{-1, -1};

    QTimer m_timer; // runs only while the widget is shown
    QElapsedTimer m_clock;
    double m_time = 0;      // seconds since start, drives ambient animation
    double m_stateTime = 0; // ms since the current screen was entered
    QTransform m_transform;
    QVector<QRectF> m_hitRects; // menu item rects of the last frame, in logical coordinates

    QVector<Particle> m_particles;
    QVector<FloatText> m_texts;
    QVector<Ring> m_rings;
    QVector<Star> m_stars;
    double m_shake = 0, m_levelPulse = 0, m_eatPulse = 0;
    double m_shownScore = 0;
};
