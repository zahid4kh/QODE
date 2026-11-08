// Snake rules for the game panel.
#include "game/SnakeGame.h"

#include <algorithm>
#include <cmath>

namespace {

QPoint offset(SnakeGame::Dir d)
{
    switch (d) {
    case SnakeGame::Up:
        return {0, -1};
    case SnakeGame::Down:
        return {0, 1};
    case SnakeGame::Left:
        return {-1, 0};
    case SnakeGame::Right:
    default:
        return {1, 0};
    }
}

bool opposite(SnakeGame::Dir a, SnakeGame::Dir b)
{
    return (a + 2) % 4 == b;
}

} // namespace

SnakeGame::SnakeGame()
    : m_rng(std::random_device{}())
{
    reset(Normal, true);
}

void SnakeGame::reset(int speed, bool walls)
{
    m_speed = std::clamp(speed, int(Slow), int(Fast));
    m_walls = walls;
    m_body.clear();
    const int y = Height / 2;
    for (int i = 0; i < 4; ++i)
        m_body.push_back(QPoint(7 - i, y));
    m_prevTail = m_body.back();
    m_queue.clear();
    m_dir = Right;
    m_moved = false;
    m_acc = 0;
    m_grow = 0;
    m_score = m_foods = m_bonuses = 0;
    m_over = m_won = false;
    m_bonus = QPoint(-1, -1);
    m_bonusLeft = 0;
    m_events.clear();
    m_food = QPoint(-1, -1);
    m_food = freeCell();
    m_foodAge = 1000; // already there when the countdown starts, not popping in
}

double SnakeGame::stepMs() const
{
    static const double base[] = {165, 125, 92};
    return std::max(48.0, base[m_speed] * std::pow(0.93, level() - 1));
}

double SnakeGame::stepProgress() const
{
    if (!m_moved)
        return 1.0;
    return std::clamp(m_acc / stepMs(), 0.0, 1.0);
}

void SnakeGame::turn(Dir dir)
{
    const Dir last = m_queue.empty() ? m_dir : m_queue.back();
    if (dir == last || opposite(dir, last) || m_queue.size() >= 3)
        return;
    m_queue.push_back(dir);
}

void SnakeGame::update(double ms)
{
    if (m_over)
        return;
    m_foodAge += ms;
    if (m_bonusLeft > 0) {
        m_bonusLeft -= ms;
        if (m_bonusLeft <= 0) {
            m_bonusLeft = 0;
            m_events.append({SnakeEvent::BonusGone, m_bonus});
            m_bonus = QPoint(-1, -1);
        }
    }
    m_acc += ms;
    // At most a few steps per frame, so a long frame (a stall) cannot run the snake into a wall unseen.
    for (int i = 0; i < 3 && m_acc >= stepMs() && !m_over; ++i) {
        m_acc -= stepMs();
        step();
    }
    if (m_acc > stepMs())
        m_acc = stepMs();
}

bool SnakeGame::occupied(QPoint cell, bool ignoreTail) const
{
    const int n = int(m_body.size()) - (ignoreTail ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        if (m_body[size_t(i)] == cell)
            return true;
    }
    return false;
}

void SnakeGame::step()
{
    if (!m_queue.empty()) {
        m_dir = m_queue.front();
        m_queue.pop_front();
    }
    QPoint next = m_body.front() + offset(m_dir);
    const bool outside = next.x() < 0 || next.y() < 0 || next.x() >= Width || next.y() >= Height;
    if (outside) {
        if (m_walls) {
            m_over = true;
            m_events.append({SnakeEvent::Death, m_body.front()});
            return;
        }
        next = QPoint((next.x() + Width) % Width, (next.y() + Height) % Height);
    }
    // The tail moves away this step unless the snake is growing, so the head may take its cell.
    if (occupied(next, m_grow == 0)) {
        m_over = true;
        m_events.append({SnakeEvent::Death, next});
        return;
    }

    m_prevTail = m_body.back();
    m_body.push_front(next);
    if (m_grow > 0) {
        --m_grow;
        m_prevTail = m_body.back(); // a growing tail stays where it is
    } else {
        m_body.pop_back();
    }
    m_moved = true;

    static const int speedFactor[] = {1, 2, 3};
    if (next == m_food) {
        const int before = level();
        ++m_foods;
        ++m_grow;
        const int points = 10 * before * speedFactor[m_speed];
        m_score += points;
        m_events.append({SnakeEvent::Eat, next, points});
        if (level() > before)
            m_events.append({SnakeEvent::LevelUp, next});
        m_food = freeCell();
        m_foodAge = 0;
        if (m_food.x() < 0) {
            m_over = m_won = true;
            m_events.append({SnakeEvent::Win, next});
            return;
        }
        // Now and then a bonus appears for a few seconds.
        if (m_bonusLeft <= 0 && m_foods >= 3 && std::uniform_int_distribution<int>(0, 3)(m_rng) == 0) {
            m_bonus = freeCell();
            if (m_bonus.x() >= 0) {
                m_bonusLeft = BonusMs;
                m_events.append({SnakeEvent::BonusSpawn, m_bonus});
            }
        }
    } else if (m_bonusLeft > 0 && next == m_bonus) {
        // Worth more the sooner it is reached.
        const int points = int((30 + 70 * bonusLeft()) * level() * speedFactor[m_speed]) / 10 * 10;
        ++m_bonuses;
        m_grow += 2;
        m_score += points;
        m_events.append({SnakeEvent::EatBonus, next, points});
        m_bonus = QPoint(-1, -1);
        m_bonusLeft = 0;
    }
}

QPoint SnakeGame::freeCell()
{
    QVector<QPoint> free;
    free.reserve(Width * Height);
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            const QPoint c(x, y);
            if (!occupied(c, false) && c != m_food && !(m_bonusLeft > 0 && c == m_bonus))
                free.append(c);
        }
    }
    if (free.isEmpty())
        return QPoint(-1, -1);
    return free[std::uniform_int_distribution<int>(0, int(free.size()) - 1)(m_rng)];
}

QVector<SnakeEvent> SnakeGame::takeEvents()
{
    QVector<SnakeEvent> events;
    events.swap(m_events);
    return events;
}
