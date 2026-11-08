// The game panel's block-stacking game (from the standalone teqris project): rules only.
#include "game/TeqrisGame.h"

#include "game/Tetromino.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr double LockDelayMs = 500.0;
constexpr int MaxLockResets = 15;
constexpr double ClearDelayMs = 380.0;
} // namespace

TeqrisGame::TeqrisGame()
    : m_rng(std::random_device{}())
{
    reset(1);
}

void TeqrisGame::reset(int startLevel)
{
    for (auto &row : m_grid)
        row.fill(-1);
    m_bag.clear();
    m_queue.clear();
    m_events.clear();

    m_startLevel = std::clamp(startLevel, 1, MaxLevel);
    m_level = m_startLevel;
    m_score = m_lines = m_pieces = m_quads = 0;
    m_combo = -1;
    m_b2b = false;
    m_hold = -1;
    m_holdUsed = false;
    m_over = false;
    m_clearing = false;
    m_clearRows.clear();
    m_hasActive = false;

    refillQueue();
    spawn(nextFromQueue());
}

void TeqrisGame::update(double ms)
{
    if (m_over)
        return;
    if (m_clearing) {
        m_clearTimer += ms;
        if (m_clearTimer >= ClearDelayMs)
            finishClear();
        return;
    }
    if (!m_hasActive)
        return;

    m_gravityAcc += ms;
    const double interval = gravityInterval();
    for (int guard = 0; m_gravityAcc >= interval && guard < Height; ++guard) {
        m_gravityAcc -= interval;
        if (!stepDown()) {
            m_gravityAcc = 0;
            break;
        }
    }

    if (onGround()) {
        m_lockTimer += ms;
        if (m_lockTimer >= LockDelayMs)
            lockPiece();
    } else {
        m_lockTimer = 0;
    }
}

bool TeqrisGame::softDrop()
{
    if (!canControl() || !stepDown())
        return false;
    m_score += 1;
    m_gravityAcc = 0;
    return true;
}

void TeqrisGame::hardDrop()
{
    if (!canControl())
        return;
    const int startY = m_y;
    while (stepDown()) {
    }
    const int distance = m_y - startY;
    m_score += 2 * distance;

    TeqrisEvent e(TeqrisEvent::HardDrop);
    e.piece = m_type;
    for (const QPoint &c : activeCells())
        e.cells.append(c);
    e.distance = distance;
    m_events.append(e);

    lockPiece();
}

bool TeqrisGame::rotate(int direction)
{
    if (!canControl() || m_type == Tetromino::O)
        return false;
    const int to = (m_rot + (direction > 0 ? 1 : 3)) % 4;
    const auto &kick = Tetromino::kicks(m_type, m_rot, to);
    for (int i = 0; i < 5; ++i) {
        const int nx = m_x + kick[i].x();
        const int ny = m_y - kick[i].y(); // tables are y-up
        if (collides(nx, ny, to))
            continue;
        m_x = nx;
        m_y = ny;
        m_rot = to;
        m_lastRotate = true;
        m_lastKick = i;
        if (m_y > m_lowestY) {
            m_lowestY = m_y;
            m_lockResets = 0;
        }
        afterShift();
        return true;
    }
    return false;
}

bool TeqrisGame::hold()
{
    if (!canControl() || m_holdUsed)
        return false;
    const int current = m_type;
    if (m_hold < 0) {
        m_hold = current;
        spawn(nextFromQueue());
    } else {
        spawn(std::exchange(m_hold, current));
    }
    m_holdUsed = true;
    m_events.append(TeqrisEvent(TeqrisEvent::Hold));
    return true;
}

std::array<QPoint, 4> TeqrisGame::ghostCells() const
{
    int y = m_y;
    while (!collides(m_x, y + 1, m_rot))
        ++y;
    return cellsAt(m_x, y, m_rot);
}

double TeqrisGame::lockProgress() const
{
    if (!m_hasActive || !onGround())
        return 0;
    return std::min(1.0, m_lockTimer / LockDelayMs);
}

QVector<int> TeqrisGame::preview() const
{
    QVector<int> result;
    for (int i = 0; i < PreviewCount && i < int(m_queue.size()); ++i)
        result.append(m_queue[i]);
    return result;
}

double TeqrisGame::clearProgress() const
{
    return m_clearing ? std::min(1.0, m_clearTimer / ClearDelayMs) : 0.0;
}

QList<TeqrisEvent> TeqrisGame::takeEvents()
{
    return std::exchange(m_events, {});
}

std::array<QPoint, 4> TeqrisGame::cellsAt(int x, int y, int rot) const
{
    auto cells = Tetromino::cells(m_type, rot);
    for (QPoint &c : cells)
        c += QPoint(x, y);
    return cells;
}

bool TeqrisGame::collides(int x, int y, int rot) const
{
    for (const QPoint &c : cellsAt(x, y, rot)) {
        if (c.x() < 0 || c.x() >= Width || c.y() >= Height)
            return true;
        if (c.y() >= 0 && m_grid[c.y()][c.x()] >= 0)
            return true;
    }
    return false;
}

bool TeqrisGame::shift(int dx)
{
    if (!canControl() || collides(m_x + dx, m_y, m_rot))
        return false;
    m_x += dx;
    m_lastRotate = false;
    afterShift();
    return true;
}

bool TeqrisGame::stepDown()
{
    if (collides(m_x, m_y + 1, m_rot))
        return false;
    ++m_y;
    m_lastRotate = false;
    if (m_y > m_lowestY) {
        m_lowestY = m_y;
        m_lockResets = 0;
    }
    return true;
}

// Moving or rotating a grounded piece restarts the lock delay, a limited number of times.
void TeqrisGame::afterShift()
{
    if (m_lockTimer > 0 && m_lockResets < MaxLockResets) {
        m_lockTimer = 0;
        ++m_lockResets;
    }
}

void TeqrisGame::spawn(int type)
{
    m_type = type;
    m_rot = 0;
    m_x = type == Tetromino::O ? 4 : 3;
    m_y = type == Tetromino::I ? 0 : 1;
    m_hasActive = true;
    m_gravityAcc = 0;
    m_lockTimer = 0;
    m_lockResets = 0;
    m_lastRotate = false;
    m_lastKick = 0;

    if (collides(m_x, m_y, m_rot)) {
        m_hasActive = false;
        gameOver();
        return;
    }
    if (!collides(m_x, m_y + 1, m_rot))
        ++m_y;
    m_lowestY = m_y;
}

int TeqrisGame::nextFromQueue()
{
    const int type = m_queue.front();
    m_queue.pop_front();
    refillQueue();
    return type;
}

// 7-bag randomizer: every run of seven pieces contains each tetromino once.
void TeqrisGame::refillQueue()
{
    while (int(m_queue.size()) < PreviewCount + 1) {
        if (m_bag.empty()) {
            for (int t = 0; t < Tetromino::Count; ++t)
                m_bag.push_back(t);
            std::shuffle(m_bag.begin(), m_bag.end(), m_rng);
        }
        m_queue.push_back(m_bag.back());
        m_bag.pop_back();
    }
}

void TeqrisGame::lockPiece()
{
    const auto cells = activeCells();
    bool allHidden = true;
    TeqrisEvent lockEvent(TeqrisEvent::Lock);
    lockEvent.piece = m_type;
    for (const QPoint &c : cells) {
        if (c.y() >= 0)
            m_grid[c.y()][c.x()] = m_type;
        if (c.y() >= Hidden)
            allHidden = false;
        lockEvent.cells.append(c);
    }
    m_hasActive = false;
    ++m_pieces;
    m_events.append(lockEvent);

    if (allHidden) {
        gameOver();
        return;
    }

    // T-spin: the T was rotated into place and three of the four corners around its centre are blocked.
    bool tspin = false, mini = false;
    if (m_type == Tetromino::T && m_lastRotate) {
        auto occupied = [this](int cx, int cy) {
            return cx < 0 || cx >= Width || cy >= Height || (cy >= 0 && m_grid[cy][cx] >= 0);
        };
        const bool corner[4] = {occupied(m_x, m_y), occupied(m_x + 2, m_y), occupied(m_x + 2, m_y + 2),
                                occupied(m_x, m_y + 2)};
        const int count = corner[0] + corner[1] + corner[2] + corner[3];
        if (count >= 3) {
            const bool front = corner[m_rot] && corner[(m_rot + 1) % 4];
            tspin = true;
            mini = !front && m_lastKick != 4;
        }
    }

    QVector<int> rows;
    for (int y = 0; y < Height; ++y) {
        if (std::all_of(m_grid[y].begin(), m_grid[y].end(), [](int v) { return v >= 0; }))
            rows.append(y);
    }
    const int n = rows.size();

    static const int normalPoints[5] = {0, 100, 300, 500, 800};
    static const int tspinPoints[4] = {400, 800, 1200, 1600};
    static const int miniPoints[3] = {100, 200, 400};
    int base = normalPoints[n];
    if (tspin)
        base = mini ? miniPoints[std::min(n, 2)] : tspinPoints[std::min(n, 3)];

    const bool difficult = n > 0 && (n == 4 || tspin);
    const bool backToBack = difficult && m_b2b;
    if (n > 0)
        m_b2b = difficult;

    int points = base * m_level;
    if (backToBack)
        points = points * 3 / 2;
    if (n > 0) {
        ++m_combo;
        points += 50 * m_combo * m_level;
    } else {
        m_combo = -1;
    }

    bool perfect = false;
    if (n > 0) {
        perfect = true;
        for (int y = 0; y < Height && perfect; ++y) {
            if (rows.contains(y))
                continue;
            perfect = std::all_of(m_grid[y].begin(), m_grid[y].end(), [](int v) { return v < 0; });
        }
        static const int perfectPoints[5] = {0, 800, 1200, 1800, 2000};
        if (perfect)
            points += perfectPoints[n] * m_level;
    }
    m_score += points;
    if (n == 4)
        ++m_quads;

    if (n > 0 || tspin) {
        TeqrisEvent e(TeqrisEvent::LineClear);
        e.rows = rows;
        e.lines = n;
        e.tspin = tspin;
        e.mini = mini;
        e.backToBack = backToBack;
        e.perfectClear = perfect;
        e.combo = n > 0 ? m_combo : 0;
        e.points = points;
        m_events.append(e);
    }

    m_holdUsed = false;
    if (n > 0) {
        m_clearing = true;
        m_clearTimer = 0;
        m_clearRows = rows;
    } else {
        spawn(nextFromQueue());
    }
}

void TeqrisGame::finishClear()
{
    std::array<std::array<int, Width>, Height> grid;
    for (auto &row : grid)
        row.fill(-1);
    int dst = Height - 1;
    for (int y = Height - 1; y >= 0; --y) {
        if (!m_clearRows.contains(y))
            grid[dst--] = m_grid[y];
    }
    m_grid = grid;

    m_lines += m_clearRows.size();
    const int newLevel = std::min(MaxLevel, m_startLevel + m_lines / 10);
    if (newLevel > m_level) {
        m_level = newLevel;
        m_events.append(TeqrisEvent(TeqrisEvent::LevelUp));
    }

    m_clearing = false;
    m_clearRows.clear();
    spawn(nextFromQueue());
}

void TeqrisGame::gameOver()
{
    m_over = true;
    m_events.append(TeqrisEvent(TeqrisEvent::GameOver));
}

// Seconds per row from the guideline gravity curve.
double TeqrisGame::gravityInterval() const
{
    return std::pow(0.8 - (m_level - 1) * 0.007, m_level - 1) * 1000.0;
}
