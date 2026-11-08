// Snake in the game panel: drawing, animation and input.
#include "game/SnakeWidget.h"

#include "game/GameArt.h"

#include <QFocusEvent>
#include <QFontMetricsF>
#include <QHideEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QShowEvent>

#include <algorithm>
#include <cmath>

using namespace GameArt;

namespace {

constexpr qreal LogicalW = 820, LogicalH = 760;
constexpr qreal CenterX = LogicalW / 2;
constexpr qreal Cell = 28;
constexpr qreal FieldW = Cell * SnakeGame::Width, FieldH = Cell * SnakeGame::Height;
constexpr qreal FieldX = (LogicalW - FieldW) / 2, FieldY = 124;

constexpr double Pi = 3.14159265358979323846;
constexpr double CountdownMs = 2200, DyingMs = 1200, ResultsLockMs = 500;

const QColor FoodColor(255, 64, 120);
const QColor Red(255, 70, 120);

const char *const SpeedNames[] = {"SLOW", "NORMAL", "FAST"};
const char *const SpeedIds[] = {"slow", "normal", "fast"};

// The step from a to b on the field, taking the shortest way round when the snake wraps over an edge.
QPointF wrapDelta(QPointF a, QPointF b)
{
    QPointF d = b - a;
    if (d.x() > 1.5)
        d.rx() -= SnakeGame::Width;
    else if (d.x() < -1.5)
        d.rx() += SnakeGame::Width;
    if (d.y() > 1.5)
        d.ry() -= SnakeGame::Height;
    else if (d.y() < -1.5)
        d.ry() += SnakeGame::Height;
    return d;
}

} // namespace

SnakeWidget::SnakeWidget(QWidget *parent)
    : QWidget(parent)
    , m_store(QStringLiteral("snake"))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_best = m_store.bestScore();
    const QString speed = m_store.option(QStringLiteral("speed"), QStringLiteral("normal")).toString();
    for (int i = 0; i < 3; ++i) {
        if (speed == QLatin1String(SpeedIds[i]))
            m_speed = i;
    }
    m_walls = m_store.option(QStringLiteral("walls"), true).toBool();
    m_game.reset(m_speed, m_walls);

    for (int i = 0; i < 110; ++i)
        m_stars.append({rnd(0, 1), rnd(0, 1), rnd(0.4, 1.5), rnd(0.5, 2.5), rnd(0, 6.28)});

    connect(&m_timer, &QTimer::timeout, this, &SnakeWidget::tick);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
}

QSize SnakeWidget::sizeHint() const
{
    return QSize(int(LogicalW), int(LogicalH));
}

// ---------------------------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------------------------

void SnakeWidget::tick()
{
    const double dt = std::min<double>(50.0, m_clock.restart());
    m_time += dt / 1000.0;
    m_stateTime += dt;

    switch (m_screen) {
    case Screen::Countdown:
        if (m_stateTime >= CountdownMs)
            setScreen(Screen::Playing);
        break;
    case Screen::Playing:
        m_playMs += dt;
        m_game.update(dt);
        processGameEvents();
        break;
    case Screen::Dying:
        if (m_stateTime >= DyingMs)
            setScreen(Screen::GameOver);
        break;
    default:
        break;
    }

    updateEffects(dt);
    update();
}

void SnakeWidget::setScreen(Screen screen)
{
    m_screen = screen;
    m_stateTime = 0;
    m_selected = 0;
}

void SnakeWidget::startGame()
{
    m_game.reset(m_speed, m_walls);
    m_game.takeEvents();
    m_particles.clear();
    m_texts.clear();
    m_rings.clear();
    m_shownScore = 0;
    m_shake = m_levelPulse = m_eatPulse = 0;
    m_newBest = false;
    m_playMs = 0;
    m_crash = QPoint(-1, -1);
    setScreen(Screen::Countdown);
}

QStringList SnakeWidget::menuItems() const
{
    switch (m_screen) {
    case Screen::Menu:
        return {QStringLiteral("play"), QStringLiteral("speed"), QStringLiteral("walls"), QStringLiteral("controls"),
                QStringLiteral("games")};
    case Screen::Controls:
        return {QStringLiteral("back")};
    case Screen::Paused:
        return {QStringLiteral("resume"), QStringLiteral("restart"), QStringLiteral("menu")};
    case Screen::GameOver:
        return {QStringLiteral("retry"), QStringLiteral("menu")};
    default:
        return {};
    }
}

void SnakeWidget::activate(const QString &item)
{
    if (item == QLatin1String("play") || item == QLatin1String("restart") || item == QLatin1String("retry")) {
        startGame();
    } else if (item == QLatin1String("speed")) {
        changeSpeed(1);
    } else if (item == QLatin1String("walls")) {
        toggleWalls();
    } else if (item == QLatin1String("controls")) {
        setScreen(Screen::Controls);
    } else if (item == QLatin1String("games")) {
        emit exitRequested();
    } else if (item == QLatin1String("resume")) {
        setScreen(m_resumeScreen);
    } else if (item == QLatin1String("menu") || item == QLatin1String("back")) {
        const bool fromControls = m_screen == Screen::Controls;
        setScreen(Screen::Menu);
        if (fromControls)
            m_selected = 3;
    }
}

void SnakeWidget::changeSpeed(int delta)
{
    m_speed = (m_speed + delta + 3) % 3;
    m_store.setOption(QStringLiteral("speed"), QLatin1String(SpeedIds[m_speed]));
}

void SnakeWidget::toggleWalls()
{
    m_walls = !m_walls;
    m_store.setOption(QStringLiteral("walls"), m_walls);
}

void SnakeWidget::handleMenuKey(int key)
{
    const QStringList items = menuItems();
    if (items.isEmpty())
        return;
    const int n = items.size();
    const QString current = items.value(m_selected);

    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_W:
        m_selected = (m_selected + n - 1) % n;
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
    case Qt::Key_Tab:
        m_selected = (m_selected + 1) % n;
        break;
    case Qt::Key_Left:
    case Qt::Key_A:
    case Qt::Key_Right:
    case Qt::Key_D: {
        const int delta = (key == Qt::Key_Left || key == Qt::Key_A) ? -1 : 1;
        if (current == QLatin1String("speed"))
            changeSpeed(delta);
        else if (current == QLatin1String("walls"))
            toggleWalls();
        break;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activate(current);
        break;
    case Qt::Key_Escape:
    case Qt::Key_P:
        if (m_screen == Screen::Paused)
            activate(QStringLiteral("resume"));
        else if (m_screen == Screen::Controls || m_screen == Screen::GameOver)
            activate(QStringLiteral("menu"));
        else if (m_screen == Screen::Menu && key == Qt::Key_Escape)
            emit exitRequested();
        break;
    default:
        break;
    }
}

// Arrows and WASD steer; returns false for other keys.
bool SnakeWidget::steer(int key)
{
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_W:
        m_game.turn(SnakeGame::Up);
        return true;
    case Qt::Key_Down:
    case Qt::Key_S:
        m_game.turn(SnakeGame::Down);
        return true;
    case Qt::Key_Left:
    case Qt::Key_A:
        m_game.turn(SnakeGame::Left);
        return true;
    case Qt::Key_Right:
    case Qt::Key_D:
        m_game.turn(SnakeGame::Right);
        return true;
    default:
        return false;
    }
}

void SnakeWidget::processGameEvents()
{
    for (const SnakeEvent &e : m_game.takeEvents()) {
        const QPointF at = cellCenter(e.cell);
        switch (e.type) {
        case SnakeEvent::Eat:
            burst(at, FoodColor, 14, 220);
            m_rings.append({at, FoodColor});
            m_texts.append({QStringLiteral("+%1").arg(e.points), at, TextBright, 18});
            m_eatPulse = 1;
            break;
        case SnakeEvent::EatBonus:
            burst(at, Gold, 26, 300);
            m_rings.append({at, Gold, 0, 0.6});
            m_texts.append({QStringLiteral("+%1").arg(e.points), at, Gold, 26});
            m_eatPulse = 1;
            m_shake = std::max(m_shake, 4.0);
            break;
        case SnakeEvent::BonusSpawn:
            m_rings.append({at, Gold});
            break;
        case SnakeEvent::BonusGone:
            burst(at, withAlpha(Gold, 0.6), 8, 90);
            break;
        case SnakeEvent::LevelUp:
            m_texts.append({QStringLiteral("LEVEL %1").arg(m_game.level()), QPointF(CenterX, FieldY + FieldH * 0.42),
                            accent(), 34, 0, 1.4});
            m_levelPulse = 1;
            break;
        case SnakeEvent::Death:
        case SnakeEvent::Win: {
            const QJsonObject stats{{QStringLiteral("length"), m_game.length()},
                                    {QStringLiteral("food"), m_game.foods()},
                                    {QStringLiteral("bonus"), m_game.bonuses()},
                                    {QStringLiteral("level"), m_game.level()},
                                    {QStringLiteral("speed"), QLatin1String(SpeedIds[m_speed])},
                                    {QStringLiteral("walls"), m_walls},
                                    {QStringLiteral("won"), e.type == SnakeEvent::Win}};
            m_newBest = m_store.recordGame(m_game.score(), stats, qint64(m_playMs));
            m_best = m_store.bestScore();
            if (e.type == SnakeEvent::Death) {
                m_crash = e.cell;
                burst(at, Red, 40, 380);
                m_shake = 12;
            } else {
                for (int i = 0; i < 6; ++i)
                    burst(cellCenter(QPoint(int(rnd(0, SnakeGame::Width)), int(rnd(0, SnakeGame::Height)))), Gold,
                          16, 260);
            }
            setScreen(Screen::Dying);
            break;
        }
        }
    }
}

// Leaving the game (focus elsewhere, panel closed, back to the picker) pauses it, so the snake never runs unseen.
void SnakeWidget::pauseIfRunning()
{
    if (m_screen == Screen::Playing || m_screen == Screen::Countdown) {
        m_resumeScreen = m_screen;
        setScreen(Screen::Paused);
    }
}

// ---------------------------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------------------------

void SnakeWidget::burst(QPointF pos, QColor color, int count, qreal speed)
{
    for (int i = 0; i < count; ++i) {
        const double angle = rnd(0, 6.2832), v = rnd(0.3, 1.0) * speed;
        m_particles.append({pos, QPointF(std::cos(angle) * v, std::sin(angle) * v), color.lighter(120), 0,
                            rnd(0.35, 0.8), rnd(1.4, 3.2)});
    }
}

void SnakeWidget::updateEffects(double ms)
{
    const double s = ms / 1000.0;

    for (Particle &pt : m_particles) {
        pt.age += s;
        pt.vel *= 0.94;
        pt.pos += pt.vel * s;
    }
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
                                     [](const Particle &pt) { return pt.age >= pt.life; }),
                      m_particles.end());
    for (FloatText &t : m_texts)
        t.age += s;
    m_texts.erase(std::remove_if(m_texts.begin(), m_texts.end(), [](const FloatText &t) { return t.age >= t.life; }),
                  m_texts.end());
    for (Ring &r : m_rings)
        r.age += s;
    m_rings.erase(std::remove_if(m_rings.begin(), m_rings.end(), [](const Ring &r) { return r.age >= r.life; }),
                  m_rings.end());

    m_shake = std::max(0.0, m_shake - ms * 0.035);
    m_levelPulse = std::max(0.0, m_levelPulse - s * 0.8);
    m_eatPulse = std::max(0.0, m_eatPulse - s * 4);

    const double target = m_game.score();
    m_shownScore += (target - m_shownScore) * std::min(1.0, ms * 0.012);
    if (std::abs(target - m_shownScore) < 1)
        m_shownScore = target;
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

QPointF SnakeWidget::cellCenter(QPointF cell) const
{
    return QPointF(FieldX + (cell.x() + 0.5) * Cell, FieldY + (cell.y() + 0.5) * Cell);
}

// The accent colour drifts from green through the spectrum as the level goes up.
QColor SnakeWidget::accent() const
{
    const bool inGame = m_screen != Screen::Menu && m_screen != Screen::Controls;
    const int level = inGame ? m_game.level() : 1;
    return QColor::fromHsv(int(std::fmod(150 + (level - 1) * 31, 360)), 200, 255);
}

QColor SnakeWidget::bodyColor(qreal along) const
{
    const QColor a = accent();
    const qreal hue = std::fmod(a.hsvHueF() + along * 0.2, 1.0);
    return QColor::fromHsvF(hue, 0.62 + 0.2 * along, 1.0 - 0.25 * along);
}

void SnakeWidget::paintButton(QPainter &p, const QRectF &rect, const QString &text, bool selected)
{
    m_hitRects.append(rect);
    drawButton(p, rect, text, selected, accent(), m_time);
}

void SnakeWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    paintBackground(p);

    const qreal scale = std::min(width() / LogicalW, height() / LogicalH);
    QTransform t;
    t.translate((width() - LogicalW * scale) / 2, (height() - LogicalH * scale) / 2);
    t.scale(scale, scale);
    m_transform = t;
    p.setTransform(t);
    m_hitRects.clear();

    switch (m_screen) {
    case Screen::Menu:
        paintMenu(p);
        break;
    case Screen::Controls:
        paintControls(p);
        break;
    default:
        p.save();
        if (m_shake > 0.05)
            p.translate(rnd(-1, 1) * m_shake, rnd(-1, 1) * m_shake);
        paintHud(p);
        paintField(p);
        paintEffects(p);
        p.restore();
        if (m_screen == Screen::Countdown)
            paintCountdown(p);
        else if (m_screen == Screen::Paused)
            paintPause(p);
        else if (m_screen == Screen::GameOver)
            paintGameOver(p);
        break;
    }
}

void SnakeWidget::paintBackground(QPainter &p)
{
    const qreal w = width(), h = height();
    const QColor acc = accent();

    QLinearGradient bg(0, 0, 0, h);
    bg.setColorAt(0, QColor(3, 10, 20));
    bg.setColorAt(0.6, QColor(5, 24, 34));
    bg.setColorAt(1, QColor(2, 8, 14));
    p.fillRect(rect(), bg);

    QRadialGradient haze(QPointF(w * 0.5, h * 0.62), std::max(w, h) * 0.6);
    haze.setColorAt(0, withAlpha(acc, 0.16));
    haze.setColorAt(1, withAlpha(acc, 0));
    p.fillRect(rect(), haze);

    p.setPen(Qt::NoPen);
    for (const Star &s : m_stars) {
        const qreal twinkle = 0.25 + 0.75 * (0.5 + 0.5 * std::sin(m_time * s.speed + s.phase));
        p.setBrush(withAlpha(QColor(210, 255, 240), twinkle * 0.7));
        p.drawEllipse(QPointF(s.x * w, s.y * h), s.size, s.size);
    }

    // A slow hex-dot lattice drifting upwards.
    const qreal step = 34;
    const qreal shift = std::fmod(m_time * 8, step * 2);
    p.setBrush(withAlpha(acc, 0.07));
    for (qreal y = -step * 2 + (step * 2 - shift); y < h + step; y += step) {
        const bool odd = int(std::floor((y + shift) / step)) % 2;
        for (qreal x = odd ? step / 2 : 0; x < w + step; x += step)
            p.drawEllipse(QPointF(x, y), 1.4, 1.4);
    }
}

void SnakeWidget::paintMenuSnakes(QPainter &p, qreal alpha)
{
    // Three decorative snakes slithering across the screen.
    struct Deco
    {
        qreal y, amp, freq, speed, offset, size;
        int dir;
    };
    static const Deco decos[] = {{120, 26, 0.018, 90, 0, 13, 1},
                                 {470, 34, 0.014, 70, 520, 16, -1},
                                 {705, 22, 0.022, 110, 260, 11, 1}};
    const qreal span = LogicalW + 600;
    for (const Deco &d : decos) {
        const qreal headX = std::fmod(m_time * d.speed + d.offset, span) - 300;
        const int segments = 22;
        for (int i = segments; i >= 0; --i) {
            qreal x = headX - i * d.size * 0.9;
            if (d.dir < 0)
                x = LogicalW - x;
            const qreal y = d.y + d.amp * std::sin((headX - i * d.size * 0.9) * d.freq + m_time * 2);
            const qreal along = qreal(i) / segments;
            const qreal r = d.size * (1 - 0.55 * along);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(bodyColor(along), alpha * (1 - 0.6 * along)));
            p.drawEllipse(QPointF(x, y), r, r);
        }
    }
}

void SnakeWidget::paintMenu(QPainter &p)
{
    paintMenuSnakes(p, 0.22);

    const qreal bob = std::sin(m_time * 1.6) * 4;
    const QPointF titlePos(CenterX, 172 + bob);
    const QFont titleFont = displayFont(100, 12);
    drawGlowText(p, titlePos + QPointF(-3, 0), QStringLiteral("SNAKE"), titleFont, withAlpha(QColor(0, 240, 255), 0.45),
                 Qt::transparent, 0);
    drawGlowText(p, titlePos + QPointF(3, 0), QStringLiteral("SNAKE"), titleFont, withAlpha(QColor(180, 255, 60), 0.4),
                 Qt::transparent, 0);
    QLinearGradient titleFill(0, titlePos.y() - 45, 0, titlePos.y() + 45);
    titleFill.setColorAt(0, QColor(200, 255, 170));
    titleFill.setColorAt(0.5, QColor(255, 255, 255));
    titleFill.setColorAt(0.52, QColor(90, 255, 190));
    titleFill.setColorAt(1, QColor(0, 190, 255));
    drawGlowText(p, titlePos, QStringLiteral("SNAKE"), titleFont, titleFill, QColor(60, 255, 170), 22);

    drawText(p, QRectF(0, 240, LogicalW, 24), QStringLiteral("NEON  SERPENT"), bodyFont(15, 8),
             withAlpha(TextBright, 0.7));

    const QStringList items = menuItems();
    for (int i = 0; i < items.size(); ++i) {
        const QRectF r(CenterX - 170, 296 + i * 60, 340, 48);
        QString label;
        if (items[i] == QLatin1String("play"))
            label = QStringLiteral("PLAY");
        else if (items[i] == QLatin1String("speed"))
            label = QStringLiteral("SPEED  %1").arg(QLatin1String(SpeedNames[m_speed]));
        else if (items[i] == QLatin1String("walls"))
            label = m_walls ? QStringLiteral("WALLS  ON") : QStringLiteral("WALLS  OFF");
        else if (items[i] == QLatin1String("controls"))
            label = QStringLiteral("CONTROLS");
        else
            label = QStringLiteral("ALL GAMES");
        paintButton(p, r, label, i == m_selected);
    }

    drawText(p, QRectF(0, 610, LogicalW, 20), QStringLiteral("HIGH SCORE"), displayFont(12, 5, QFont::Bold),
             withAlpha(Gold, 0.75));
    drawGlowText(p, QPointF(CenterX, 646), QString::number(m_best), displayFont(30, 3), Gold, Gold, 10);
    if (m_store.gamesPlayed() > 0)
        drawText(p, QRectF(0, 670, LogicalW, 20),
                 QStringLiteral("%1 %2  ·  %3 PLAYED")
                     .arg(m_store.gamesPlayed())
                     .arg(m_store.gamesPlayed() == 1 ? QStringLiteral("GAME") : QStringLiteral("GAMES"),
                          playTime(m_store.totalPlayMs())),
                 bodyFont(12, 2), withAlpha(TextDim, 0.8));

    drawText(p, QRectF(0, 716, LogicalW, 20),
             QStringLiteral("↑ ↓  NAVIGATE     ← →  CHANGE     ENTER  SELECT     ESC  GAMES"), bodyFont(13, 1),
             withAlpha(TextDim, 0.9));
}

void SnakeWidget::paintControls(QPainter &p)
{
    paintMenuSnakes(p, 0.12);

    const QColor acc = accent();
    drawGlowText(p, QPointF(CenterX, 84), QStringLiteral("CONTROLS"), displayFont(46, 6), TextBright, acc, 16);

    const QRectF panel(110, 132, 600, 480);
    drawPanel(p, panel, QString(), acc);
    drawNeonFrame(p, panel, withAlpha(acc, 0.5), 10, 0.5);

    struct Row
    {
        QStringList keys;
        QString action;
    };
    const Row rows[] = {
        {{QStringLiteral("↑"), QStringLiteral("←"), QStringLiteral("↓"), QStringLiteral("→")}, QStringLiteral("STEER")},
        {{QStringLiteral("W"), QStringLiteral("A"), QStringLiteral("S"), QStringLiteral("D")}, QStringLiteral("STEER")},
        {{QStringLiteral("ESC"), QStringLiteral("P")}, QStringLiteral("PAUSE")},
        {{QStringLiteral("ENTER")}, QStringLiteral("SELECT")},
    };
    qreal y = panel.top() + 40;
    const QFont keyFont = bodyFont(15, 1);
    for (const Row &row : rows) {
        qreal total = 0;
        for (const QString &k : row.keys)
            total += std::max<qreal>(34, QFontMetricsF(keyFont).horizontalAdvance(k) + 20) + 8;
        qreal x = 390 - total;
        for (const QString &k : row.keys)
            x += drawKeyCap(p, QPointF(x, y), k, acc) + 8;
        drawText(p, QRectF(415, y, 260, 32), row.action, displayFont(15, 3, QFont::Bold), TextBright,
                 Qt::AlignLeft | Qt::AlignVCenter);
        y += 50;
    }

    // How to play.
    p.setPen(QPen(withAlpha(acc, 0.25), 1));
    p.drawLine(QPointF(panel.left() + 40, y + 6), QPointF(panel.right() - 40, y + 6));
    y += 26;
    struct Tip
    {
        QColor dot;
        QString text;
    };
    const Tip tips[] = {
        {FoodColor, QStringLiteral("EAT FOOD TO GROW AND SCORE")},
        {Gold, QStringLiteral("GOLD BONUS FADES: THE SOONER, THE MORE POINTS")},
        {acc, QStringLiteral("EVERY 5 FOODS: NEXT LEVEL, FASTER SNAKE")},
        {TextDim, QStringLiteral("WALLS OFF: WRAP AROUND THE EDGES")},
        {QColor(120, 220, 255), QStringLiteral("FASTER SPEED: UP TO 3x POINTS")},
    };
    for (const Tip &tip : tips) {
        p.setPen(Qt::NoPen);
        p.setBrush(tip.dot);
        p.drawEllipse(QPointF(panel.left() + 56, y + 12), 5, 5);
        drawText(p, QRectF(panel.left() + 74, y, panel.width() - 110, 24), tip.text, bodyFont(13, 1), TextBright,
                 Qt::AlignLeft | Qt::AlignVCenter);
        y += 30;
    }

    paintButton(p, QRectF(CenterX - 130, 640, 260, 50), QStringLiteral("BACK"), true);
}

void SnakeWidget::paintHud(QPainter &p)
{
    const QColor acc = accent();
    const qreal top = 22, h = 86, gap = 12;
    const qreal widths[] = {232, 160, 160, FieldW - 232 - 160 - 160 - 3 * gap};
    qreal x = FieldX;
    QRectF boxes[4];
    for (int i = 0; i < 4; ++i) {
        boxes[i] = QRectF(x, top, widths[i], h);
        drawPanel(p, boxes[i], QString(), acc);
        x += widths[i] + gap;
    }
    auto label = [&](const QRectF &box, const QString &text) {
        drawText(p, QRectF(box.left() + 16, box.top() + 10, box.width() - 32, 18), text,
                 displayFont(11, 4, QFont::Bold), withAlpha(acc, 0.85), Qt::AlignLeft | Qt::AlignVCenter);
    };

    label(boxes[0], QStringLiteral("SCORE"));
    drawGlowText(p, QPointF(boxes[0].left() + 16, boxes[0].top() + 54), QString::number(qint64(m_shownScore)),
                 displayFont(30, 1), TextBright, acc, 8, Qt::AlignLeft);

    label(boxes[1], QStringLiteral("BEST"));
    drawText(p, QRectF(boxes[1].left() + 16, boxes[1].top() + 36, boxes[1].width() - 32, 36),
             QString::number(std::max(m_best, m_game.score())), displayFont(22, 1, QFont::Bold), Gold,
             Qt::AlignLeft | Qt::AlignVCenter);

    label(boxes[2], QStringLiteral("LENGTH"));
    drawText(p, QRectF(boxes[2].left() + 16, boxes[2].top() + 36, boxes[2].width() - 32, 36),
             QString::number(m_game.length()), displayFont(22, 1), TextBright, Qt::AlignLeft | Qt::AlignVCenter);

    label(boxes[3], QStringLiteral("LEVEL"));
    drawGlowText(p, QPointF(boxes[3].left() + 16, boxes[3].top() + 50),
                 QStringLiteral("%1").arg(m_game.level(), 2, 10, QLatin1Char('0')), displayFont(26, 2), TextBright, acc,
                 6 + 14 * m_levelPulse, Qt::AlignLeft);
    // Progress towards the next level.
    const QRectF bar(boxes[3].left() + 16, boxes[3].bottom() - 16, boxes[3].width() - 32, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 20));
    p.drawRoundedRect(bar, 3, 3);
    const qreal progress = (m_game.foods() % SnakeGame::FoodsPerLevel) / qreal(SnakeGame::FoodsPerLevel);
    if (progress > 0) {
        QLinearGradient fill(bar.topLeft(), bar.topRight());
        fill.setColorAt(0, acc);
        fill.setColorAt(1, Magenta);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * progress, bar.height()), 3, 3);
    }

    // Mode line under the field.
    drawText(p, QRectF(FieldX, FieldY + FieldH + 10, FieldW, 20),
             QStringLiteral("%1  ·  WALLS %2").arg(QLatin1String(SpeedNames[m_game.speed()]),
                                                   m_game.walls() ? QStringLiteral("ON") : QStringLiteral("OFF")),
             bodyFont(12, 2), withAlpha(TextDim, 0.85), Qt::AlignLeft | Qt::AlignVCenter);
    drawText(p, QRectF(FieldX, FieldY + FieldH + 10, FieldW, 20),
             QStringLiteral("ARROWS / WASD  STEER     ESC  PAUSE"), bodyFont(12, 1), withAlpha(TextDim, 0.85),
             Qt::AlignRight | Qt::AlignVCenter);
}

void SnakeWidget::paintField(QPainter &p)
{
    const QColor acc = accent();
    const QRectF field(FieldX, FieldY, FieldW, FieldH);

    if (m_game.walls()) {
        drawNeonFrame(p, field.adjusted(-5, -5, 5, 5), acc, 8, 1.0 + m_levelPulse * 2.5);
    } else {
        // No walls: a dashed, softer frame (the edges are portals).
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(withAlpha(acc, 0.18), 8));
        p.drawRoundedRect(field.adjusted(-5, -5, 5, 5), 8, 8);
        QPen dash(withAlpha(acc.lighter(130), 0.85), 2, Qt::DashLine);
        dash.setDashOffset(-m_time * 6);
        p.setPen(dash);
        p.drawRoundedRect(field.adjusted(-5, -5, 5, 5), 8, 8);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(4, 12, 20, 235));
    p.drawRoundedRect(field.adjusted(-4, -4, 4, 4), 7, 7);

    p.save();
    p.setClipRect(field);

    // Checkerboard.
    p.setBrush(QColor(255, 255, 255, 7));
    for (int y = 0; y < SnakeGame::Height; ++y) {
        for (int x = (y % 2); x < SnakeGame::Width; x += 2)
            p.drawRect(QRectF(FieldX + x * Cell, FieldY + y * Cell, Cell, Cell));
    }

    paintFood(p);
    paintSnake(p);

    if (m_levelPulse > 0)
        p.fillRect(field, withAlpha(acc, m_levelPulse * 0.08));
    p.restore();
}

void SnakeWidget::paintFood(QPainter &p)
{
    const bool dying = m_screen == Screen::Dying || m_screen == Screen::GameOver;

    if (m_game.food().x() >= 0) {
        const QPointF c = cellCenter(m_game.food());
        const qreal pop = easeOutBack(std::min(1.0, m_game.foodAge() / 350.0));
        const qreal pulse = 1 + 0.08 * std::sin(m_time * 6);
        const qreal r = Cell * 0.36 * pop * pulse;

        p.save();
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        QRadialGradient glow(c, Cell * 1.3);
        glow.setColorAt(0, withAlpha(FoodColor, dying ? 0.15 : 0.4));
        glow.setColorAt(1, withAlpha(FoodColor, 0));
        p.fillRect(QRectF(c - QPointF(Cell * 1.3, Cell * 1.3), QSizeF(Cell * 2.6, Cell * 2.6)), glow);
        p.restore();

        QRadialGradient orb(c - QPointF(r * 0.35, r * 0.35), r * 1.4);
        orb.setColorAt(0, FoodColor.lighter(170));
        orb.setColorAt(0.5, FoodColor);
        orb.setColorAt(1, FoodColor.darker(170));
        p.setPen(Qt::NoPen);
        p.setBrush(orb);
        p.drawEllipse(c, r, r);
        // Leaf.
        p.save();
        p.translate(c + QPointF(r * 0.25, -r * 0.95));
        p.rotate(-35);
        p.setBrush(QColor(90, 230, 120));
        p.drawEllipse(QPointF(0, 0), r * 0.42, r * 0.2);
        p.restore();
        p.setBrush(withAlpha(Qt::white, 0.55));
        p.drawEllipse(c - QPointF(r * 0.38, r * 0.38), r * 0.2, r * 0.14);
    }

    if (m_game.hasBonus()) {
        const QPointF c = cellCenter(m_game.bonus());
        const qreal left = m_game.bonusLeft();
        // Blinks in its last seconds.
        const bool visible = left > 0.3 || std::fmod(m_time, 0.25) < 0.16;
        if (visible) {
            p.save();
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            QRadialGradient glow(c, Cell * 1.5);
            glow.setColorAt(0, withAlpha(Gold, 0.45));
            glow.setColorAt(1, withAlpha(Gold, 0));
            p.fillRect(QRectF(c - QPointF(Cell * 1.5, Cell * 1.5), QSizeF(Cell * 3, Cell * 3)), glow);
            p.restore();

            // A spinning star.
            const qreal outer = Cell * 0.44, inner = outer * 0.45;
            QPainterPath star;
            for (int i = 0; i < 10; ++i) {
                const qreal a = m_time * 1.8 + i * Pi / 5 - Pi / 2;
                const qreal rr = (i % 2) ? inner : outer;
                const QPointF pt = c + QPointF(std::cos(a) * rr, std::sin(a) * rr);
                if (i == 0)
                    star.moveTo(pt);
                else
                    star.lineTo(pt);
            }
            star.closeSubpath();
            QRadialGradient fill(c, outer);
            fill.setColorAt(0, QColor(255, 250, 210));
            fill.setColorAt(1, Gold);
            p.setPen(QPen(Gold.darker(150), 1));
            p.setBrush(fill);
            p.drawPath(star);
        }
        // Time left.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(withAlpha(Gold, 0.85), 2.2, Qt::SolidLine, Qt::RoundCap));
        const QRectF arc(c - QPointF(Cell * 0.62, Cell * 0.62), QSizeF(Cell * 1.24, Cell * 1.24));
        p.drawArc(arc, 90 * 16, int(-360 * 16 * left));
    }
}

void SnakeWidget::paintSnake(QPainter &p)
{
    const std::deque<QPoint> &body = m_game.body();
    const int n = int(body.size());
    if (n < 2)
        return;
    const qreal t = m_game.stepProgress();

    // The snake as a polyline through the cell centres, with the head sliding from the previous head cell and the tail
    // sliding off its previous cell, so it moves smoothly between steps. Unwrapped over the edges (walls off).
    QVector<QPointF> pts;
    pts.reserve(n + 1);
    const QPointF second(body[1]);
    pts.append(second + wrapDelta(second, QPointF(body[0])) * t);
    pts.append(second);
    for (int i = 2; i < n; ++i)
        pts.append(pts.last() + wrapDelta(QPointF(body[size_t(i - 1)]), QPointF(body[size_t(i)])));
    const QPointF tailCell(body[size_t(n - 1)]);
    const QPointF tailEnd = pts.last() + wrapDelta(tailCell, QPointF(m_game.previousTail())) * (1 - t);
    if (tailEnd != pts.last())
        pts.append(tailEnd);
    for (QPointF &pt : pts)
        pt = cellCenter(pt);

    const bool dying = m_screen == Screen::Dying || m_screen == Screen::GameOver;
    const qreal deathT = m_screen == Screen::Dying ? std::min(1.0, m_stateTime / (DyingMs * 0.75)) : dying ? 1.0 : 0.0;
    const bool won = m_game.won();
    auto colorAt = [&](qreal along) {
        QColor c = bodyColor(along);
        if (dying && !won && along <= deathT) {
            const int g = 120 + int(30 * (1 - along));
            c = QColor(g, g - 6, g + 20);
        } else if (won) {
            c = QColor::fromHsvF(float(std::fmod(along + m_time * 0.5, 1.0)), 0.6f, 1.0f);
        }
        return c;
    };

    // Draw at the offsets the snake reaches when it wraps over the edges.
    QVector<QPointF> shifts{QPointF(0, 0)};
    if (!m_game.walls()) {
        QRectF bounds(pts.first(), QSizeF(0, 0));
        for (const QPointF &pt : pts)
            bounds = bounds.united(QRectF(pt, QSizeF(0.01, 0.01)));
        const QRectF field(FieldX, FieldY, FieldW, FieldH);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if ((dx || dy) && bounds.translated(dx * FieldW, dy * FieldH).adjusted(-Cell, -Cell, Cell, Cell)
                                      .intersects(field))
                    shifts.append(QPointF(dx * FieldW, dy * FieldH));
            }
        }
    }

    const int m = int(pts.size());
    const qreal width = Cell * 0.74;
    const QColor head = colorAt(0);
    for (const QPointF &shift : shifts) {
        p.save();
        p.translate(shift);

        // Glow.
        QPainterPath path(pts[0]);
        for (int i = 1; i < m; ++i)
            path.lineTo(pts[i]);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(withAlpha(head, dying ? 0.06 : 0.14), Cell * 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // Body, tail first so the segments nearer the head sit on top.
        for (int i = m - 1; i >= 1; --i) {
            const qreal along = qreal(i) / (m - 1);
            const qreal w = width * (1 - 0.35 * along * along);
            p.setPen(QPen(colorAt(along), w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(pts[i], pts[i - 1]);
        }
        // Shine along the back.
        p.setPen(QPen(withAlpha(Qt::white, 0.22), width * 0.18, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path.translated(-width * 0.14, -width * 0.14));
        // Scales: a darker dot on every cell.
        p.setPen(Qt::NoPen);
        for (int i = 2; i < m - 1; ++i) {
            const qreal along = qreal(i) / (m - 1);
            p.setBrush(withAlpha(colorAt(along).darker(140), 0.5));
            p.drawEllipse(pts[i], width * 0.14, width * 0.14);
        }

        // Head.
        QPointF dir = pts[0] - pts[1];
        const qreal len = std::hypot(dir.x(), dir.y());
        if (len < 0.01) {
            static const QPointF dirs[] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            dir = dirs[m_game.direction()];
        } else {
            dir /= len;
        }
        const QPointF perp(-dir.y(), dir.x());
        const qreal hr = Cell * 0.46 * (1 + 0.16 * m_eatPulse);
        const QPointF hc = pts[0];

        // Tongue, now and then.
        if (!dying && std::fmod(m_time, 1.6) < 0.22) {
            const QPointF base = hc + dir * hr * 0.9;
            const QPointF tip = base + dir * Cell * 0.32;
            p.setPen(QPen(QColor(255, 70, 110), 2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(base, tip);
            p.drawLine(tip, tip + (dir + perp) * Cell * 0.1);
            p.drawLine(tip, tip + (dir - perp) * Cell * 0.1);
            p.setPen(Qt::NoPen);
        }

        QRadialGradient hg(hc - dir * hr * 0.2 - perp * hr * 0.2, hr * 1.4);
        hg.setColorAt(0, head.lighter(135));
        hg.setColorAt(1, head.darker(115));
        p.setBrush(hg);
        p.drawEllipse(hc, hr, hr);

        // Eyes (crossed out once it has crashed).
        for (int side : {-1, 1}) {
            const QPointF eye = hc + dir * hr * 0.25 + perp * side * hr * 0.45;
            if (dying && !won) {
                p.setPen(QPen(QColor(30, 20, 40), 2, Qt::SolidLine, Qt::RoundCap));
                const qreal e = hr * 0.18;
                p.drawLine(eye + QPointF(-e, -e), eye + QPointF(e, e));
                p.drawLine(eye + QPointF(-e, e), eye + QPointF(e, -e));
                p.setPen(Qt::NoPen);
            } else {
                p.setBrush(Qt::white);
                p.drawEllipse(eye, hr * 0.27, hr * 0.27);
                p.setBrush(QColor(15, 10, 30));
                p.drawEllipse(eye + dir * hr * 0.1, hr * 0.14, hr * 0.14);
            }
        }
        p.restore();
    }

    // The crash spot.
    if (dying && !won && m_crash.x() >= 0) {
        const QPointF c = cellCenter(m_crash);
        const qreal k = std::min(1.0, m_stateTime / 400);
        p.setPen(QPen(withAlpha(Red, 1 - 0.6 * k), 3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, Cell * (0.4 + 0.8 * k), Cell * (0.4 + 0.8 * k));
    }
}

void SnakeWidget::paintEffects(QPainter &p)
{
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.setBrush(Qt::NoBrush);
    for (const Ring &r : m_rings) {
        const qreal k = r.age / r.life;
        p.setPen(QPen(withAlpha(r.color, 1 - k), 3 * (1 - k) + 0.5));
        const qreal radius = Cell * (0.4 + 1.6 * k);
        p.drawEllipse(r.pos, radius, radius);
    }
    p.setPen(Qt::NoPen);
    for (const Particle &pt : m_particles) {
        const qreal k = 1 - pt.age / pt.life;
        p.setBrush(withAlpha(pt.color, k));
        p.drawEllipse(pt.pos, pt.size * (0.5 + 0.5 * k), pt.size * (0.5 + 0.5 * k));
    }
    p.restore();

    for (const FloatText &t : m_texts) {
        const double k = t.age / t.life;
        const qreal scale = k < 0.18 ? easeOutBack(k / 0.18) : 1.0;
        const qreal alpha = k > 0.6 ? 1 - (k - 0.6) / 0.4 : 1.0;
        p.save();
        p.translate(t.pos + QPointF(0, -40 * k));
        p.scale(scale, scale);
        drawGlowText(p, QPointF(0, 0), t.text, displayFont(t.size, 2), withAlpha(t.color, alpha),
                     withAlpha(t.color, alpha), t.size * 0.45);
        p.restore();
    }
}

void SnakeWidget::paintCountdown(QPainter &p)
{
    QString text;
    double frac;
    if (m_stateTime < 1800) {
        text = QString::number(3 - int(m_stateTime / 600));
        frac = std::fmod(m_stateTime, 600) / 600;
    } else {
        text = QStringLiteral("GO!");
        frac = (m_stateTime - 1800) / 400;
    }
    frac = std::clamp(frac, 0.0, 1.0);
    const qreal scale = 1.7 - 0.7 * (1 - std::pow(1 - frac, 3));
    const qreal alpha = 1 - std::pow(frac, 3);
    p.save();
    p.translate(CenterX, FieldY + FieldH / 2);
    p.scale(scale, scale);
    drawGlowText(p, QPointF(0, 0), text, displayFont(80, 4), withAlpha(TextBright, alpha), withAlpha(accent(), alpha),
                 26);
    p.restore();
    drawText(p, QRectF(FieldX, FieldY + FieldH * 0.68, FieldW, 24), QStringLiteral("STEER WITH ARROWS OR WASD"),
             bodyFont(14, 3), withAlpha(TextBright, 0.75));
}

void SnakeWidget::paintPause(QPainter &p)
{
    const QColor acc = accent();
    p.fillRect(QRectF(-2000, -2000, 5000, 5000), QColor(2, 8, 14, 190));

    const QRectF panel(250, 200, 320, 340);
    drawPanel(p, panel, QString(), acc);
    drawNeonFrame(p, panel, withAlpha(acc, 0.7), 10, 0.8);
    drawGlowText(p, QPointF(CenterX, panel.top() + 62), QStringLiteral("PAUSED"), displayFont(40, 6), TextBright, acc,
                 16);

    static const char *const labels[] = {"RESUME", "RESTART", "MAIN MENU"};
    for (int i = 0; i < 3; ++i)
        paintButton(p, QRectF(CenterX - 125, panel.top() + 130 + i * 64, 250, 48), QLatin1String(labels[i]),
                    i == m_selected);
}

void SnakeWidget::paintGameOver(QPainter &p)
{
    const qreal fade = std::min(1.0, m_stateTime / 450);
    const qreal slide = (1 - easeOutBack(fade)) * 40;
    p.fillRect(QRectF(-2000, -2000, 5000, 5000), QColor(2, 8, 14, int(200 * fade)));

    p.save();
    p.translate(0, slide);
    p.setOpacity(fade);

    const bool won = m_game.won();
    const QColor frame = won ? Gold : Red;
    const QRectF panel(200, 120, 420, 530);
    drawPanel(p, panel, QString(), frame);
    drawNeonFrame(p, panel, withAlpha(frame, 0.8), 10, 0.9);

    const QString title = won ? QStringLiteral("YOU WIN!") : QStringLiteral("GAME OVER");
    const QFont titleFont = displayFont(44, 4);
    const QPointF titlePos(CenterX, panel.top() + 70);
    const qreal jitter = std::fmod(m_time, 2.2) < 0.12 ? rnd(-4, 4) : 0;
    drawGlowText(p, titlePos + QPointF(-3 + jitter, 0), title, titleFont, withAlpha(QColor(0, 230, 255), 0.5),
                 Qt::transparent, 0);
    drawGlowText(p, titlePos + QPointF(3 - jitter, 0), title, titleFont, withAlpha(QColor(120, 255, 90), 0.45),
                 Qt::transparent, 0);
    drawGlowText(p, titlePos, title, titleFont, TextBright, frame, 20);

    if (m_newBest) {
        const qreal blink = 0.55 + 0.45 * std::sin(m_time * 7);
        drawGlowText(p, QPointF(CenterX, panel.top() + 118), QStringLiteral("NEW HIGH SCORE!"), displayFont(18, 3),
                     withAlpha(Gold, blink), withAlpha(Gold, blink), 10);
    }

    struct Stat
    {
        QString label;
        QString value;
        QColor color;
    };
    const Stat stats[] = {
        {QStringLiteral("SCORE"), QString::number(m_game.score()), TextBright},
        {QStringLiteral("LENGTH"), QString::number(m_game.length()), TextBright},
        {QStringLiteral("FOOD"),
         m_game.bonuses() > 0 ? QStringLiteral("%1  +%2 ★").arg(m_game.foods()).arg(m_game.bonuses())
                              : QString::number(m_game.foods()),
         TextBright},
        {QStringLiteral("LEVEL"), QString::number(m_game.level()), TextBright},
        {QStringLiteral("BEST"), QString::number(m_best), Gold},
    };
    qreal y = panel.top() + 150;
    for (const Stat &s : stats) {
        drawText(p, QRectF(panel.left() + 50, y, 160, 34), s.label, displayFont(14, 4, QFont::Bold), TextDim,
                 Qt::AlignLeft | Qt::AlignVCenter);
        drawText(p, QRectF(panel.left() + 170, y, 200, 34), s.value, displayFont(22, 1), s.color,
                 Qt::AlignRight | Qt::AlignVCenter);
        p.setPen(QPen(QColor(255, 255, 255, 18), 1));
        p.drawLine(QPointF(panel.left() + 50, y + 38), QPointF(panel.right() - 50, y + 38));
        y += 44;
    }
    drawText(p, QRectF(panel.left(), y + 2, panel.width(), 20),
             QStringLiteral("%1  ·  WALLS %2").arg(QLatin1String(SpeedNames[m_game.speed()]),
                                                   m_game.walls() ? QStringLiteral("ON") : QStringLiteral("OFF")),
             bodyFont(12, 2), withAlpha(TextDim, 0.85));

    paintButton(p, QRectF(CenterX - 130, panel.top() + 400, 260, 48), QStringLiteral("RETRY"), m_selected == 0);
    paintButton(p, QRectF(CenterX - 130, panel.top() + 460, 260, 48), QStringLiteral("MAIN MENU"), m_selected == 1);
    p.restore();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void SnakeWidget::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();

    switch (m_screen) {
    case Screen::Playing:
    case Screen::Countdown: // a turn pressed during the countdown is taken on the first step
        if (event->isAutoRepeat())
            break;
        if (key == Qt::Key_Escape || key == Qt::Key_P) {
            m_resumeScreen = m_screen;
            setScreen(Screen::Paused);
        } else {
            steer(key);
        }
        break;
    case Screen::Dying:
        break;
    case Screen::GameOver:
        // Don't let a steering key pressed in the last moment skip the results.
        if (m_stateTime > ResultsLockMs)
            handleMenuKey(key);
        break;
    default:
        handleMenuKey(key);
        break;
    }
}

void SnakeWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = m_transform.inverted().map(event->position());
    for (int i = 0; i < m_hitRects.size(); ++i) {
        if (m_hitRects[i].contains(pos))
            m_selected = i;
    }
}

void SnakeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || (m_screen == Screen::GameOver && m_stateTime < ResultsLockMs))
        return;
    const QPointF pos = m_transform.inverted().map(event->position());
    const QStringList items = menuItems();
    for (int i = 0; i < m_hitRects.size() && i < items.size(); ++i) {
        if (!m_hitRects[i].contains(pos))
            continue;
        m_selected = i;
        if (items[i] == QLatin1String("speed"))
            changeSpeed(pos.x() < m_hitRects[i].center().x() ? -1 : 1);
        else
            activate(items[i]);
        return;
    }
}

void SnakeWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    pauseIfRunning();
}

// Plain keys are the game's, not the window's (see TeqrisWidget::event); Ctrl / Alt combinations still reach it.
bool SnakeWidget::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

// Tab moves through the menus instead of focusing the next widget.
bool SnakeWidget::focusNextPrevChild(bool)
{
    return false;
}

void SnakeWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_clock.restart();
    m_timer.start();
}

void SnakeWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    pauseIfRunning();
    m_timer.stop();
}
