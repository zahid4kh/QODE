#pragma once

#include <QList>
#include <QPoint>
#include <QVector>

#include <array>
#include <deque>
#include <random>
#include <vector>

// Something that happened inside the game which the view may want to animate.
struct TeqrisEvent
{
    enum Type { Lock, HardDrop, LineClear, LevelUp, Hold, GameOver };

    explicit TeqrisEvent(Type t)
        : type(t)
    {
    }

    Type type;
    int piece = -1;
    QVector<QPoint> cells; // Lock / HardDrop: final cells of the piece
    int distance = 0;      // HardDrop: rows travelled
    QVector<int> rows;     // LineClear: cleared rows
    int lines = 0;
    bool tspin = false;
    bool mini = false;
    bool backToBack = false;
    bool perfectClear = false;
    int combo = 0;
    int points = 0;
};

// Teqris rules: board, pieces, gravity, locking, scoring. Knows nothing about drawing or input.
class TeqrisGame
{
public:
    static constexpr int Width = 10;
    static constexpr int Height = 22; // includes the hidden rows above the visible field
    static constexpr int Hidden = 2;
    static constexpr int Visible = Height - Hidden;
    static constexpr int PreviewCount = 5;
    static constexpr int MaxLevel = 20;

    TeqrisGame();

    void reset(int startLevel);
    void update(double ms);

    bool moveLeft() { return shift(-1); }
    bool moveRight() { return shift(1); }
    bool softDrop();
    void hardDrop();
    bool rotate(int direction); // +1 clockwise, -1 counter-clockwise
    bool hold();

    int cell(int x, int y) const { return m_grid[y][x]; }
    bool hasActive() const { return m_hasActive; }
    int activeType() const { return m_type; }
    std::array<QPoint, 4> activeCells() const { return cellsAt(m_x, m_y, m_rot); }
    std::array<QPoint, 4> ghostCells() const;
    double lockProgress() const;

    int holdType() const { return m_hold; }
    bool holdUsed() const { return m_holdUsed; }
    QVector<int> preview() const;

    bool isClearing() const { return m_clearing; }
    const QVector<int> &clearingRows() const { return m_clearRows; }
    double clearProgress() const;

    bool isOver() const { return m_over; }
    int score() const { return m_score; }
    int lines() const { return m_lines; }
    int level() const { return m_level; }
    int pieces() const { return m_pieces; }
    int quads() const { return m_quads; } // four-line clears

    QList<TeqrisEvent> takeEvents();

private:
    std::array<QPoint, 4> cellsAt(int x, int y, int rot) const;
    bool collides(int x, int y, int rot) const;
    bool canControl() const { return m_hasActive && !m_over && !m_clearing; }
    bool onGround() const { return collides(m_x, m_y + 1, m_rot); }
    bool shift(int dx);
    bool stepDown();
    void afterShift();
    void spawn(int type);
    int nextFromQueue();
    void refillQueue();
    void lockPiece();
    void finishClear();
    void gameOver();
    double gravityInterval() const;

    std::array<std::array<int, Width>, Height> m_grid;
    std::mt19937 m_rng;
    std::vector<int> m_bag;
    std::deque<int> m_queue;
    QList<TeqrisEvent> m_events;

    bool m_hasActive = false;
    int m_type = 0, m_rot = 0, m_x = 0, m_y = 0;
    int m_lowestY = 0;
    double m_gravityAcc = 0;
    double m_lockTimer = 0;
    int m_lockResets = 0;
    bool m_lastRotate = false;
    int m_lastKick = 0;

    int m_hold = -1;
    bool m_holdUsed = false;

    bool m_clearing = false;
    double m_clearTimer = 0;
    QVector<int> m_clearRows;

    bool m_over = false;
    int m_startLevel = 1, m_level = 1;
    int m_score = 0, m_lines = 0, m_combo = -1;
    bool m_b2b = false;
    int m_pieces = 0, m_quads = 0;
};
