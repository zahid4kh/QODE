#pragma once

#include "game/GameStore.h"
#include "game/TeqrisGame.h"

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

// Teqris, the game panel's block-stacking game: draws every screen (menu, controls, game, pause, game over) with
// QPainter and turns keyboard/mouse input into game actions. All drawing happens in an 820x760 logical space that is
// scaled to fit the widget. The game has its own neon look in every theme, so its colours are not Theme fields.
// It only ticks while shown, pauses when it loses focus or is hidden, and keeps its scores in GameStore("teqris").
class TeqrisWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TeqrisWidget(QWidget *parent = nullptr);

    QSize sizeHint() const override;

signals:
    void closeRequested(); // the menu's CLOSE item

protected:
    bool event(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
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
        double age = 0, life = 1.4;
    };
    struct Trail
    {
        QRectF rect;
        QColor color;
        double age = 0, life = 0.28;
    };
    struct Star
    {
        qreal x, y, size, speed, phase;
    };
    struct DecoPiece
    {
        int type;
        QPointF pos;
        qreal speed, angle, spin, cell, alpha;
    };

    // flow
    void tick();
    void setScreen(Screen screen);
    void startGame();
    QStringList menuItems() const;
    void activate(const QString &item);
    void changeLevel(int delta);
    void handleMenuKey(int key);
    void handleGameKey(int key);
    void releaseAllKeys();
    void processGameEvents();
    void pauseIfRunning();

    // effects
    void updateEffects(double ms);
    void respawnDeco(DecoPiece &piece, bool anywhere);
    void addText(const QString &text, QColor color, qreal size, qreal yOffset);

    // drawing
    void paintBackground(QPainter &p);
    void paintMenu(QPainter &p);
    void paintControls(QPainter &p);
    void paintGame(QPainter &p);
    void paintWell(QPainter &p);
    void paintSidePanels(QPainter &p);
    void paintEffects(QPainter &p);
    void paintCountdown(QPainter &p);
    void paintPause(QPainter &p);
    void paintGameOver(QPainter &p);
    void paintButton(QPainter &p, const QRectF &rect, const QString &text, bool selected);
    QRectF cellRect(int x, int y) const;
    QColor accent() const;

    TeqrisGame m_game;
    Screen m_screen = Screen::Menu;
    Screen m_resumeScreen = Screen::Playing;
    int m_selected = 0;
    int m_startLevel = 1;
    GameStore m_store;
    int m_best = 0;
    bool m_newBest = false;
    double m_playMs = 0; // time spent in Playing this game, for the store

    QTimer m_timer; // runs only while the widget is shown
    QElapsedTimer m_clock;
    double m_time = 0;      // seconds since start, drives ambient animation
    double m_stateTime = 0; // ms since the current screen was entered
    QTransform m_transform;
    QVector<QRectF> m_hitRects; // menu item rects of the last frame, in logical coordinates

    bool m_leftHeld = false, m_rightHeld = false, m_downHeld = false;
    int m_dasDir = 0;
    double m_dasTimer = 0, m_arrTimer = 0, m_softTimer = 0;

    QVector<Particle> m_particles;
    QVector<FloatText> m_texts;
    QVector<Trail> m_trails;
    QVector<QPoint> m_flashCells;
    double m_flashAge = 1e9;
    double m_shake = 0, m_wellFlash = 0, m_levelPulse = 0;
    double m_shownScore = 0;
    QVector<Star> m_stars;
    QVector<DecoPiece> m_deco;
};
