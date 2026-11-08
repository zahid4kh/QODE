#pragma once

#include <QPoint>
#include <QVector>

#include <deque>
#include <random>

// Something that happened inside the snake game which the view may want to animate.
struct SnakeEvent
{
    enum Type { Eat, EatBonus, BonusSpawn, BonusGone, LevelUp, Death, Win };

    Type type;
    QPoint cell;    // Eat / EatBonus / BonusSpawn / BonusGone: the food's cell; Death: where the head crashed
    int points = 0; // Eat / EatBonus
};

// Snake rules: the field, the snake, food and the bonus, speed and scoring. Knows nothing about drawing or input.
// The snake moves one cell per step; the step interval shrinks as the level (one per 5 foods) goes up.
class SnakeGame
{
public:
    static constexpr int Width = 26;
    static constexpr int Height = 21;
    static constexpr int FoodsPerLevel = 5;
    static constexpr double BonusMs = 6500; // how long the bonus stays

    enum Dir { Up, Right, Down, Left };
    enum Speed { Slow, Normal, Fast };

    SnakeGame();

    void reset(int speed, bool walls);
    void update(double ms);
    // Queues a turn (up to three ahead, so quick key presses are not lost); turning back on itself is ignored.
    void turn(Dir dir);

    const std::deque<QPoint> &body() const { return m_body; } // head first
    QPoint previousTail() const { return m_prevTail; }          // where the tail was before the last step
    double stepProgress() const;                                 // 0..1 between the last step and the next
    Dir direction() const { return m_dir; }

    QPoint food() const { return m_food; }
    double foodAge() const { return m_foodAge; } // ms since the food appeared
    bool hasBonus() const { return m_bonusLeft > 0; }
    QPoint bonus() const { return m_bonus; }
    double bonusLeft() const { return m_bonusLeft / BonusMs; } // 1 → 0

    bool walls() const { return m_walls; }
    int speed() const { return m_speed; }
    int score() const { return m_score; }
    int foods() const { return m_foods; }
    int bonuses() const { return m_bonuses; }
    int level() const { return 1 + m_foods / FoodsPerLevel; }
    int length() const { return int(m_body.size()); }
    bool over() const { return m_over; }
    bool won() const { return m_won; }
    double stepMs() const;

    QVector<SnakeEvent> takeEvents();

private:
    void step();
    QPoint freeCell(); // a random empty cell, or (-1, -1) when the field is full
    bool occupied(QPoint cell, bool ignoreTail) const;

    std::deque<QPoint> m_body;
    std::deque<Dir> m_queue;
    Dir m_dir = Right;
    QPoint m_prevTail;
    bool m_moved = false;
    double m_acc = 0;
    int m_grow = 0;

    QPoint m_food{-1, -1};
    double m_foodAge = 0;
    QPoint m_bonus{-1, -1};
    double m_bonusLeft = 0;

    int m_speed = Normal;
    bool m_walls = true;
    int m_score = 0, m_foods = 0, m_bonuses = 0;
    bool m_over = false, m_won = false;

    QVector<SnakeEvent> m_events;
    std::mt19937 m_rng;
};
