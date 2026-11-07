// Teqris in the game panel: drawing, animation and input (from the standalone teqris project).
#include "game/TeqrisWidget.h"

#include "game/GameArt.h"
#include "game/Tetromino.h"

#include <QFocusEvent>
#include <QHideEvent>
#include <QJsonObject>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMap>
#include <QRadialGradient>
#include <QSet>
#include <QShowEvent>

#include <algorithm>
#include <cmath>
#include <random>

namespace {

constexpr qreal LogicalW = 820, LogicalH = 760;
constexpr qreal Cell = 32;
constexpr qreal WellX = 250, WellY = 80;
constexpr qreal WellW = Cell * TeqrisGame::Width, WellH = Cell * TeqrisGame::Visible;
constexpr qreal CenterX = LogicalW / 2;

constexpr double DasMs = 150, ArrMs = 35, SoftDropMs = 30;
constexpr double CountdownMs = 2200, DyingMs = 1300;
constexpr int MaxStartLevel = 15;

} // namespace

using namespace GameArt;

TeqrisWidget::TeqrisWidget(QWidget *parent)
    : QWidget(parent)
    , m_store(QStringLiteral("teqris"))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_best = m_store.bestScore();
    m_startLevel = std::clamp(m_store.option(QStringLiteral("startLevel"), 1).toInt(), 1, MaxStartLevel);

    for (int i = 0; i < 130; ++i)
        m_stars.append({rnd(0, 1), rnd(0, 1), rnd(0.4, 1.6), rnd(0.5, 2.5), rnd(0, 6.28)});
    m_deco.resize(16);
    for (DecoPiece &d : m_deco)
        respawnDeco(d, true);

    connect(&m_timer, &QTimer::timeout, this, &TeqrisWidget::tick);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
}

QSize TeqrisWidget::sizeHint() const
{
    return QSize(int(LogicalW), int(LogicalH));
}

// ---------------------------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------------------------

void TeqrisWidget::tick()
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
        if (m_dasDir != 0) {
            m_dasTimer += dt;
            if (m_dasTimer >= DasMs) {
                m_arrTimer += dt;
                while (m_arrTimer >= ArrMs) {
                    m_arrTimer -= ArrMs;
                    if (!(m_dasDir < 0 ? m_game.moveLeft() : m_game.moveRight())) {
                        m_arrTimer = 0;
                        break;
                    }
                }
            }
        }
        if (m_downHeld) {
            m_softTimer += dt;
            while (m_softTimer >= SoftDropMs) {
                m_softTimer -= SoftDropMs;
                if (!m_game.softDrop()) {
                    m_softTimer = 0;
                    break;
                }
            }
        }
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

void TeqrisWidget::setScreen(Screen screen)
{
    m_screen = screen;
    m_stateTime = 0;
    m_selected = 0;
    releaseAllKeys();
}

void TeqrisWidget::startGame()
{
    m_game.reset(m_startLevel);
    m_game.takeEvents();
    m_particles.clear();
    m_texts.clear();
    m_trails.clear();
    m_flashCells.clear();
    m_shownScore = 0;
    m_shake = m_wellFlash = m_levelPulse = 0;
    m_newBest = false;
    m_playMs = 0;
    setScreen(Screen::Countdown);
}

QStringList TeqrisWidget::menuItems() const
{
    switch (m_screen) {
    case Screen::Menu:
        return {QStringLiteral("play"), QStringLiteral("level"), QStringLiteral("controls"), QStringLiteral("games")};
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

void TeqrisWidget::activate(const QString &item)
{
    if (item == QLatin1String("play") || item == QLatin1String("restart") || item == QLatin1String("retry")) {
        startGame();
    } else if (item == QLatin1String("level")) {
        changeLevel(1);
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
            m_selected = 2;
    }
}

void TeqrisWidget::changeLevel(int delta)
{
    m_startLevel += delta;
    if (m_startLevel > MaxStartLevel)
        m_startLevel = 1;
    if (m_startLevel < 1)
        m_startLevel = MaxStartLevel;
    m_store.setOption(QStringLiteral("startLevel"), m_startLevel);
}

void TeqrisWidget::handleMenuKey(int key)
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
        if (current == QLatin1String("level"))
            changeLevel(-1);
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
        if (current == QLatin1String("level"))
            changeLevel(1);
        break;
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

void TeqrisWidget::handleGameKey(int key)
{
    switch (key) {
    case Qt::Key_Left:
    case Qt::Key_A:
        m_leftHeld = true;
        m_dasDir = -1;
        m_dasTimer = m_arrTimer = 0;
        m_game.moveLeft();
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
        m_rightHeld = true;
        m_dasDir = 1;
        m_dasTimer = m_arrTimer = 0;
        m_game.moveRight();
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        m_downHeld = true;
        m_softTimer = 0;
        m_game.softDrop();
        break;
    case Qt::Key_Up:
    case Qt::Key_W:
    case Qt::Key_X:
    case Qt::Key_E:
        m_game.rotate(1);
        break;
    case Qt::Key_Z:
    case Qt::Key_Q:
    case Qt::Key_Control:
        m_game.rotate(-1);
        break;
    case Qt::Key_Space:
        m_game.hardDrop();
        break;
    case Qt::Key_C:
    case Qt::Key_Shift:
        m_game.hold();
        break;
    case Qt::Key_Escape:
    case Qt::Key_P:
        m_resumeScreen = Screen::Playing;
        setScreen(Screen::Paused);
        break;
    default:
        break;
    }
}

void TeqrisWidget::releaseAllKeys()
{
    m_leftHeld = m_rightHeld = m_downHeld = false;
    m_dasDir = 0;
    m_dasTimer = m_arrTimer = m_softTimer = 0;
}

void TeqrisWidget::processGameEvents()
{
    for (const TeqrisEvent &e : m_game.takeEvents()) {
        switch (e.type) {
        case TeqrisEvent::HardDrop: {
            m_shake = std::max(m_shake, std::min(9.0, 3.0 + e.distance * 0.35));
            QMap<int, QPair<int, int>> columns; // x -> (top, bottom)
            for (const QPoint &c : e.cells) {
                auto it = columns.find(c.x());
                if (it == columns.end())
                    columns.insert(c.x(), {c.y(), c.y()});
                else
                    *it = {std::min(it->first, c.y()), std::max(it->second, c.y())};
            }
            const QColor color = Tetromino::color(e.piece);
            for (auto it = columns.cbegin(); it != columns.cend(); ++it) {
                const QRectF top = cellRect(it.key(), it->first - e.distance);
                const QRectF bottom = cellRect(it.key(), it->first);
                if (e.distance > 0)
                    m_trails.append({QRectF(top.topLeft(), QPointF(bottom.right(), bottom.top())), color});
                const QRectF last = cellRect(it.key(), it->second);
                for (int i = 0; i < 5; ++i)
                    m_particles.append({QPointF(rnd(last.left(), last.right()), last.bottom()),
                                        QPointF(rnd(-140, 140), rnd(-220, -40)), color.lighter(130), 0,
                                        rnd(0.25, 0.5), rnd(1.2, 2.6)});
            }
            break;
        }
        case TeqrisEvent::Lock:
            m_flashCells = e.cells;
            m_flashAge = 0;
            break;
        case TeqrisEvent::LineClear: {
            for (int row : e.rows) {
                for (int x = 0; x < TeqrisGame::Width; ++x) {
                    const QRectF r = cellRect(x, row);
                    const QColor color = Tetromino::color(m_game.cell(x, row));
                    for (int i = 0; i < 3; ++i)
                        m_particles.append({r.center() + QPointF(rnd(-10, 10), rnd(-10, 10)),
                                            QPointF(rnd(-260, 260), rnd(-320, 60)), color, 0, rnd(0.5, 1.1),
                                            rnd(1.5, 3.8)});
                }
            }
            static const char *const names[] = {"", "SINGLE", "DOUBLE", "TRIPLE", "QUAD"};
            QString label;
            if (e.tspin)
                label = QStringLiteral("T-SPIN") + (e.mini ? QStringLiteral(" MINI") : QString()) +
                        (e.lines > 0 ? QStringLiteral(" ") + QLatin1String(names[e.lines]) : QString());
            else
                label = QLatin1String(names[e.lines]);

            qreal y = 0;
            if (e.backToBack) {
                addText(QStringLiteral("BACK-TO-BACK"), Gold, 16, y - 40);
            }
            if (e.lines == 4 && !e.tspin) {
                addText(label, QColor(120, 240, 255), 44, y);
                m_wellFlash = 1.0;
                m_shake = std::max(m_shake, 10.0);
            } else if (e.tspin) {
                addText(label, QColor(214, 120, 255), 30, y);
                m_wellFlash = std::max(m_wellFlash, 0.5);
            } else if (!label.isEmpty()) {
                addText(label, TextBright, e.lines >= 3 ? 30 : 24, y);
            }
            if (e.combo > 0)
                addText(QStringLiteral("COMBO x%1").arg(e.combo), Magenta, 20, y + 42);
            if (e.perfectClear)
                addText(QStringLiteral("PERFECT CLEAR"), Gold, 28, y - 80);
            if (e.points > 0)
                addText(QStringLiteral("+%1").arg(e.points), withAlpha(TextBright, 0.85), 18, y + 76);
            break;
        }
        case TeqrisEvent::LevelUp:
            addText(QStringLiteral("LEVEL %1").arg(m_game.level()), accent(), 30, -130);
            m_levelPulse = 1.0;
            break;
        case TeqrisEvent::Hold:
            break;
        case TeqrisEvent::GameOver: {
            const QJsonObject stats{{QStringLiteral("lines"), m_game.lines()},
                                    {QStringLiteral("level"), m_game.level()},
                                    {QStringLiteral("startLevel"), m_startLevel},
                                    {QStringLiteral("pieces"), m_game.pieces()},
                                    {QStringLiteral("quads"), m_game.quads()}};
            m_newBest = m_store.recordGame(m_game.score(), stats, qint64(m_playMs));
            m_best = m_store.bestScore();
            m_shake = 12;
            setScreen(Screen::Dying);
            break;
        }
        }
    }
}

// Leaving the game (focus elsewhere, panel closed) pauses it, so the pieces never fall unseen.
void TeqrisWidget::pauseIfRunning()
{
    if (m_screen == Screen::Playing || m_screen == Screen::Countdown) {
        m_resumeScreen = m_screen;
        setScreen(Screen::Paused);
    }
    releaseAllKeys();
}

// ---------------------------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------------------------

void TeqrisWidget::updateEffects(double ms)
{
    const double s = ms / 1000.0;

    for (Particle &pt : m_particles) {
        pt.age += s;
        pt.vel.ry() += 700 * s;
        pt.vel *= 0.985;
        pt.pos += pt.vel * s;
    }
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
                                     [](const Particle &pt) { return pt.age >= pt.life; }),
                      m_particles.end());

    for (FloatText &t : m_texts)
        t.age += s;
    m_texts.erase(std::remove_if(m_texts.begin(), m_texts.end(), [](const FloatText &t) { return t.age >= t.life; }),
                  m_texts.end());

    for (Trail &t : m_trails)
        t.age += s;
    m_trails.erase(std::remove_if(m_trails.begin(), m_trails.end(), [](const Trail &t) { return t.age >= t.life; }),
                   m_trails.end());

    m_flashAge += ms;
    m_shake = std::max(0.0, m_shake - ms * 0.035);
    m_wellFlash = std::max(0.0, m_wellFlash - s * 2.2);
    m_levelPulse = std::max(0.0, m_levelPulse - s * 0.8);

    const double target = m_game.score();
    m_shownScore += (target - m_shownScore) * std::min(1.0, ms * 0.012);
    if (std::abs(target - m_shownScore) < 1)
        m_shownScore = target;

    if (m_screen == Screen::Menu || m_screen == Screen::Controls) {
        for (DecoPiece &d : m_deco) {
            d.pos.ry() += d.speed * s;
            d.angle += d.spin * s;
            if (d.pos.y() > LogicalH + 90)
                respawnDeco(d, false);
        }
    }
}

void TeqrisWidget::respawnDeco(DecoPiece &piece, bool anywhere)
{
    piece.type = int(rnd(0, Tetromino::Count - 0.001));
    piece.cell = rnd(12, 30);
    piece.pos = QPointF(rnd(-20, LogicalW + 20), anywhere ? rnd(-80, LogicalH) : rnd(-160, -80));
    piece.speed = rnd(18, 55) * (piece.cell / 20.0);
    piece.angle = rnd(0, 360);
    piece.spin = rnd(-25, 25);
    piece.alpha = 0.06 + (piece.cell - 12) / 18.0 * 0.16;
}

void TeqrisWidget::addText(const QString &text, QColor color, qreal size, qreal yOffset)
{
    m_texts.append({text, QPointF(WellX + WellW / 2, WellY + WellH * 0.42 + yOffset), color, size});
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

QRectF TeqrisWidget::cellRect(int x, int y) const
{
    return QRectF(WellX + x * Cell, WellY + (y - TeqrisGame::Hidden) * Cell, Cell, Cell);
}

// The UI accent colour drifts through the spectrum as the level goes up.
QColor TeqrisWidget::accent() const
{
    const bool inGame = m_screen != Screen::Menu && m_screen != Screen::Controls;
    const int level = inGame ? m_game.level() : 1;
    return QColor::fromHsv(int(std::fmod(186 + (level - 1) * 37, 360)), 190, 255);
}

void TeqrisWidget::paintEvent(QPaintEvent *)
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
    default: {
        p.save();
        if (m_shake > 0.05)
            p.translate(rnd(-1, 1) * m_shake, rnd(-1, 1) * m_shake);
        paintGame(p);
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
}

void TeqrisWidget::paintBackground(QPainter &p)
{
    const qreal w = width(), h = height();
    const bool menu = m_screen == Screen::Menu || m_screen == Screen::Controls;

    QLinearGradient bg(0, 0, 0, h);
    bg.setColorAt(0, QColor(8, 5, 24));
    bg.setColorAt(0.6, QColor(26, 8, 48));
    bg.setColorAt(1, QColor(6, 3, 16));
    p.fillRect(rect(), bg);

    QRadialGradient haze(QPointF(w * 0.5, h * 0.64), std::max(w, h) * 0.55);
    haze.setColorAt(0, QColor(150, 40, 190, 70));
    haze.setColorAt(1, QColor(150, 40, 190, 0));
    p.fillRect(rect(), haze);

    p.setPen(Qt::NoPen);
    for (const Star &s : m_stars) {
        const qreal twinkle = 0.25 + 0.75 * (0.5 + 0.5 * std::sin(m_time * s.speed + s.phase));
        p.setBrush(withAlpha(QColor(220, 225, 255), twinkle * 0.8));
        p.drawEllipse(QPointF(s.x * w, s.y * h * 0.64), s.size, s.size);
    }

    // Synthwave floor: perspective grid scrolling towards the viewer.
    const qreal horizon = h * 0.64;
    const qreal gridAlpha = menu ? 0.5 : 0.3;
    const int rows = 16;
    for (int i = 0; i < rows; ++i) {
        const double z = (i + std::fmod(m_time * 0.7, 1.0)) / rows;
        const qreal y = horizon + (h - horizon) * z * z;
        p.setPen(QPen(withAlpha(Magenta, gridAlpha * z), 1.2));
        p.drawLine(QPointF(0, y), QPointF(w, y));
    }
    for (int k = -14; k <= 14; ++k) {
        const qreal xb = w / 2 + k * w * 0.11;
        QLinearGradient lg(0, horizon, 0, h);
        lg.setColorAt(0, withAlpha(Magenta, 0));
        lg.setColorAt(1, withAlpha(Magenta, gridAlpha * 0.8));
        p.setPen(QPen(QBrush(lg), 1.2));
        p.drawLine(QPointF(w / 2 + k * w * 0.012, horizon), QPointF(xb, h));
    }
    QLinearGradient glow(0, horizon - 30, 0, horizon + 30);
    glow.setColorAt(0, withAlpha(Magenta, 0));
    glow.setColorAt(0.5, withAlpha(QColor(255, 120, 220), gridAlpha * 0.6));
    glow.setColorAt(1, withAlpha(Magenta, 0));
    p.fillRect(QRectF(0, horizon - 30, w, 60), glow);
}

void TeqrisWidget::paintButton(QPainter &p, const QRectF &rect, const QString &text, bool selected)
{
    m_hitRects.append(rect);
    drawButton(p, rect, text, selected, accent(), m_time);
}

void TeqrisWidget::paintMenu(QPainter &p)
{
    for (const DecoPiece &d : m_deco) {
        p.save();
        p.translate(d.pos);
        p.rotate(d.angle);
        drawPiece(p, d.type, QPointF(0, 0), d.cell, d.alpha);
        p.restore();
    }

    // Title with a slight chromatic split.
    const qreal bob = std::sin(m_time * 1.6) * 4;
    const QPointF titlePos(CenterX, 190 + bob);
    const QFont titleFont = displayFont(96, 10);
    drawGlowText(p, titlePos + QPointF(-3, 0), QStringLiteral("TEQRIS"), titleFont, withAlpha(QColor(0, 240, 255), 0.45),
                 Qt::transparent, 0);
    drawGlowText(p, titlePos + QPointF(3, 0), QStringLiteral("TEQRIS"), titleFont, withAlpha(Magenta, 0.45),
                 Qt::transparent, 0);
    QLinearGradient titleFill(0, titlePos.y() - 45, 0, titlePos.y() + 45);
    titleFill.setColorAt(0, QColor(140, 250, 255));
    titleFill.setColorAt(0.5, QColor(255, 255, 255));
    titleFill.setColorAt(0.52, QColor(255, 130, 230));
    titleFill.setColorAt(1, QColor(190, 60, 255));
    drawGlowText(p, titlePos, QStringLiteral("TEQRIS"), titleFont, titleFill, QColor(255, 80, 220), 22);

    drawText(p, QRectF(0, 262, LogicalW, 24), QStringLiteral("NEON  BLOCK  STACKER"), bodyFont(15, 8),
             withAlpha(TextBright, 0.7));

    const QStringList items = menuItems();
    for (int i = 0; i < items.size(); ++i) {
        const QRectF r(CenterX - 170, 330 + i * 66, 340, 52);
        QString label;
        if (items[i] == QLatin1String("play"))
            label = QStringLiteral("PLAY");
        else if (items[i] == QLatin1String("level"))
            label = QStringLiteral("LEVEL  %1").arg(m_startLevel, 2, 10, QLatin1Char('0'));
        else if (items[i] == QLatin1String("controls"))
            label = QStringLiteral("CONTROLS");
        else
            label = QStringLiteral("ALL GAMES");
        paintButton(p, r, label, i == m_selected);
    }

    drawText(p, QRectF(0, 616, LogicalW, 20), QStringLiteral("HIGH SCORE"), displayFont(12, 5, QFont::Bold),
             withAlpha(Gold, 0.75));
    drawGlowText(p, QPointF(CenterX, 652), QString::number(m_best), displayFont(30, 3), Gold, Gold, 10);
    if (m_store.gamesPlayed() > 0)
        drawText(p, QRectF(0, 676, LogicalW, 20),
                 QStringLiteral("%1 %2  ·  %3 PLAYED")
                     .arg(m_store.gamesPlayed())
                     .arg(m_store.gamesPlayed() == 1 ? QStringLiteral("GAME") : QStringLiteral("GAMES"),
                          playTime(m_store.totalPlayMs())),
                 bodyFont(12, 2), withAlpha(TextDim, 0.8));

    drawText(p, QRectF(0, 716, LogicalW, 20), QStringLiteral("↑ ↓  NAVIGATE     ← →  LEVEL     ENTER  SELECT     ESC  GAMES"),
             bodyFont(13, 1), withAlpha(TextDim, 0.9));
}

void TeqrisWidget::paintControls(QPainter &p)
{
    for (const DecoPiece &d : m_deco) {
        p.save();
        p.translate(d.pos);
        p.rotate(d.angle);
        drawPiece(p, d.type, QPointF(0, 0), d.cell, d.alpha * 0.6);
        p.restore();
    }

    const QColor acc = accent();
    drawGlowText(p, QPointF(CenterX, 90), QStringLiteral("CONTROLS"), displayFont(46, 6), TextBright, acc, 16);

    const QRectF panel(140, 140, 540, 470);
    drawPanel(p, panel, QString(), acc);
    drawNeonFrame(p, panel, withAlpha(acc, 0.5), 10, 0.5);

    struct Row
    {
        QStringList keys;
        QString action;
    };
    const Row rows[] = {
        {{QStringLiteral("←"), QStringLiteral("→"), QStringLiteral("A"), QStringLiteral("D")}, QStringLiteral("MOVE")},
        {{QStringLiteral("↓"), QStringLiteral("S")}, QStringLiteral("SOFT DROP")},
        {{QStringLiteral("SPACE")}, QStringLiteral("HARD DROP")},
        {{QStringLiteral("↑"), QStringLiteral("W"), QStringLiteral("X")}, QStringLiteral("ROTATE RIGHT")},
        {{QStringLiteral("Z"), QStringLiteral("Q")}, QStringLiteral("ROTATE LEFT")},
        {{QStringLiteral("C"), QStringLiteral("SHIFT")}, QStringLiteral("HOLD")},
        {{QStringLiteral("ESC"), QStringLiteral("P")}, QStringLiteral("PAUSE")},
    };
    qreal y = panel.top() + 56;
    const QFont keyFont = bodyFont(15, 1);
    for (const Row &row : rows) {
        qreal total = 0;
        for (const QString &k : row.keys)
            total += std::max<qreal>(34, QFontMetricsF(keyFont).horizontalAdvance(k) + 20) + 8;
        qreal x = 400 - total;
        for (const QString &k : row.keys)
            x += drawKeyCap(p, QPointF(x, y), k, acc) + 8;
        drawText(p, QRectF(425, y, 240, 32), row.action, displayFont(15, 3, QFont::Bold), TextBright,
                 Qt::AlignLeft | Qt::AlignVCenter);
        y += 52;
    }

    paintButton(p, QRectF(CenterX - 130, 640, 260, 50), QStringLiteral("BACK"), true);
}

void TeqrisWidget::paintGame(QPainter &p)
{
    const QColor acc = accent();
    drawGlowText(p, QPointF(WellX + WellW / 2, 42), QStringLiteral("TEQRIS"), displayFont(30, 8), TextBright, acc, 12);
    paintSidePanels(p);
    paintWell(p);
    paintEffects(p);
}

void TeqrisWidget::paintSidePanels(QPainter &p)
{
    const QColor acc = accent();

    // HOLD
    const QRectF holdRect(40, 80, 180, 130);
    drawPanel(p, holdRect, QStringLiteral("HOLD"), acc);
    drawPiece(p, m_game.holdType(), QPointF(holdRect.center().x(), holdRect.top() + 82), 24,
              m_game.holdUsed() ? 0.3 : 1.0);

    // Stats
    const QRectF stats(40, 230, 180, 330);
    drawPanel(p, stats, QString(), acc);
    const qreal sx = stats.left() + 18, sw = stats.width() - 36;
    auto label = [&](qreal y, const QString &text) {
        drawText(p, QRectF(sx, y, sw, 18), text, displayFont(11, 4, QFont::Bold), withAlpha(acc, 0.85),
                 Qt::AlignLeft | Qt::AlignVCenter);
    };
    label(stats.top() + 18, QStringLiteral("SCORE"));
    drawGlowText(p, QPointF(sx, stats.top() + 56), QString::number(qint64(m_shownScore)), displayFont(26, 1),
                 TextBright, acc, 8, Qt::AlignLeft);
    label(stats.top() + 92, QStringLiteral("BEST"));
    drawText(p, QRectF(sx, stats.top() + 112, sw, 26), QString::number(std::max(m_best, m_game.score())),
             displayFont(18, 1, QFont::Bold), Gold, Qt::AlignLeft | Qt::AlignVCenter);

    label(stats.top() + 156, QStringLiteral("LEVEL"));
    drawGlowText(p, QPointF(sx, stats.top() + 192), QStringLiteral("%1").arg(m_game.level(), 2, 10, QLatin1Char('0')),
                 displayFont(30, 2), TextBright, acc, 8 + 14 * m_levelPulse, Qt::AlignLeft);

    label(stats.top() + 228, QStringLiteral("LINES"));
    drawText(p, QRectF(sx, stats.top() + 248, sw, 30), QString::number(m_game.lines()), displayFont(22, 1),
             TextBright, Qt::AlignLeft | Qt::AlignVCenter);

    // Progress towards the next level.
    const QRectF bar(sx, stats.top() + 292, sw, 8);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 20));
    p.drawRoundedRect(bar, 4, 4);
    const qreal progress = m_game.level() >= TeqrisGame::MaxLevel ? 1.0 : (m_game.lines() % 10) / 10.0;
    if (progress > 0) {
        QLinearGradient fill(bar.topLeft(), bar.topRight());
        fill.setColorAt(0, acc);
        fill.setColorAt(1, Magenta);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * progress, bar.height()), 4, 4);
    }
    drawText(p, QRectF(sx, stats.top() + 304, sw, 16), QStringLiteral("NEXT LEVEL"), bodyFont(10, 2),
             withAlpha(TextDim, 0.8), Qt::AlignRight | Qt::AlignVCenter);

    // NEXT queue
    const QRectF nextRect(600, 80, 180, 400);
    drawPanel(p, nextRect, QStringLiteral("NEXT"), acc);
    const QVector<int> next = m_game.preview();
    for (int i = 0; i < next.size(); ++i) {
        if (i == 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(acc, 0.07));
            p.drawRoundedRect(QRectF(nextRect.left() + 14, nextRect.top() + 46, nextRect.width() - 28, 84), 8, 8);
            drawPiece(p, next[i], QPointF(nextRect.center().x(), nextRect.top() + 88), 24);
        } else {
            drawPiece(p, next[i], QPointF(nextRect.center().x(), nextRect.top() + 108 + i * 64), 17,
                      1.0 - i * 0.12);
        }
    }

    // Key hints
    const QRectF keys(600, 500, 180, 220);
    drawPanel(p, keys, QStringLiteral("KEYS"), acc);
    const QPair<QString, QString> hints[] = {
        {QStringLiteral("← → A D"), QStringLiteral("MOVE")}, {QStringLiteral("↑ W"), QStringLiteral("ROTATE")},
        {QStringLiteral("↓ S"), QStringLiteral("SOFT")},     {QStringLiteral("SPACE"), QStringLiteral("DROP")},
        {QStringLiteral("C"), QStringLiteral("HOLD")},       {QStringLiteral("ESC"), QStringLiteral("PAUSE")},
    };
    qreal hy = keys.top() + 48;
    for (const auto &h : hints) {
        drawText(p, QRectF(keys.left() + 16, hy, 90, 24), h.first, bodyFont(13), TextBright,
                 Qt::AlignLeft | Qt::AlignVCenter);
        drawText(p, QRectF(keys.left() + 90, hy, 74, 24), h.second, bodyFont(12, 1), TextDim,
                 Qt::AlignRight | Qt::AlignVCenter);
        hy += 27;
    }
}

void TeqrisWidget::paintWell(QPainter &p)
{
    const QColor acc = accent();
    const QRectF well(WellX, WellY, WellW, WellH);

    drawNeonFrame(p, well.adjusted(-5, -5, 5, 5), acc, 8, 1.0 + m_levelPulse * 2.5);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(7, 5, 20, 235));
    p.drawRoundedRect(well.adjusted(-4, -4, 4, 4), 7, 7);

    p.save();
    p.setClipRect(well);

    // Grid
    p.setPen(QPen(QColor(255, 255, 255, 13), 1));
    for (int x = 1; x < TeqrisGame::Width; ++x)
        p.drawLine(QPointF(WellX + x * Cell, WellY), QPointF(WellX + x * Cell, WellY + WellH));
    for (int y = 1; y < TeqrisGame::Visible; ++y)
        p.drawLine(QPointF(WellX, WellY + y * Cell), QPointF(WellX + WellW, WellY + y * Cell));

    const bool dying = m_screen == Screen::Dying || m_screen == Screen::GameOver;
    const bool showActive = m_game.hasActive() && !dying;

    // Column guide under the falling piece
    if (showActive) {
        QSet<int> cols;
        for (const QPoint &c : m_game.activeCells())
            cols.insert(c.x());
        for (int x : cols) {
            QLinearGradient g(0, WellY, 0, WellY + WellH);
            g.setColorAt(0, withAlpha(Tetromino::color(m_game.activeType()), 0.0));
            g.setColorAt(1, withAlpha(Tetromino::color(m_game.activeType()), 0.07));
            p.fillRect(QRectF(WellX + x * Cell, WellY, Cell, WellH), g);
        }
    }

    // Locked cells
    const QVector<int> &clearing = m_game.clearingRows();
    const double clearT = m_game.clearProgress();
    const int grayRows = m_screen == Screen::Dying ? int(std::min(1.0, m_stateTime / (DyingMs * 0.8)) * (TeqrisGame::Visible + 1))
                         : m_screen == Screen::GameOver ? TeqrisGame::Visible + 1
                                                        : 0;
    for (int y = TeqrisGame::Hidden; y < TeqrisGame::Height; ++y) {
        const bool gray = TeqrisGame::Height - y <= grayRows;
        const bool clearingRow = clearing.contains(y);
        for (int x = 0; x < TeqrisGame::Width; ++x) {
            const int v = m_game.cell(x, y);
            QRectF r = cellRect(x, y);
            if (gray) {
                drawBlock(p, r, v >= 0 ? QColor(118, 110, 140) : QColor(46, 40, 66), v >= 0 ? 1.0 : 0.9);
                continue;
            }
            if (v < 0)
                continue;
            if (clearingRow) {
                const qreal shrink = std::clamp((clearT - 0.3) / 0.7, 0.0, 1.0);
                const qreal side = Cell * (1 - shrink);
                r = QRectF(r.center() - QPointF(side / 2, side / 2), QSizeF(side, side));
                if (side < 1)
                    continue;
                drawBlock(p, r, Tetromino::color(v).lighter(100 + int(80 * (1 - clearT))));
            } else {
                drawBlock(p, r, Tetromino::color(v));
            }
        }
    }

    // Line clear flash bars
    for (int y : clearing) {
        const QRectF row = cellRect(0, y);
        const qreal grow = clearT * Cell * 0.8;
        const QRectF bar(WellX, row.top() - grow / 2, WellW, Cell + grow);
        QLinearGradient g(bar.topLeft(), bar.bottomLeft());
        const qreal a = (1 - clearT) * 0.9;
        g.setColorAt(0, withAlpha(Qt::white, 0));
        g.setColorAt(0.5, withAlpha(Qt::white, a));
        g.setColorAt(1, withAlpha(Qt::white, 0));
        p.fillRect(bar, g);
    }

    if (showActive) {
        const QColor color = Tetromino::color(m_game.activeType());

        // Ghost
        for (const QPoint &c : m_game.ghostCells()) {
            const QRectF r = cellRect(c.x(), c.y()).adjusted(2, 2, -2, -2);
            p.setPen(QPen(withAlpha(color, 0.55), 1.6));
            p.setBrush(withAlpha(color, 0.10));
            p.drawRoundedRect(r, 4, 4);
        }

        // Glow, then the piece itself. It blinks toward white as the lock delay runs out.
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        for (const QPoint &c : m_game.activeCells()) {
            const QRectF r = cellRect(c.x(), c.y());
            QRadialGradient g(r.center(), Cell * 1.1);
            g.setColorAt(0, withAlpha(color, 0.30));
            g.setColorAt(1, withAlpha(color, 0));
            p.fillRect(r.adjusted(-Cell, -Cell, Cell, Cell), g);
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const qreal lock = m_game.lockProgress();
        const QColor pieceColor = lock > 0 ? color.lighter(100 + int(50 * lock * (0.5 + 0.5 * std::sin(m_time * 30))))
                                           : color;
        for (const QPoint &c : m_game.activeCells())
            drawBlock(p, cellRect(c.x(), c.y()), pieceColor);
    }

    // Hard drop trails
    for (const Trail &t : m_trails) {
        const qreal k = 1 - t.age / t.life;
        QLinearGradient g(t.rect.topLeft(), t.rect.bottomLeft());
        g.setColorAt(0, withAlpha(t.color, 0));
        g.setColorAt(1, withAlpha(t.color, 0.45 * k));
        p.fillRect(t.rect.adjusted(3 * (1 - k), 0, -3 * (1 - k), 0), g);
    }

    // Lock flash
    if (m_flashAge < 140) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(Qt::white, 0.55 * (1 - m_flashAge / 140)));
        for (const QPoint &c : m_flashCells)
            p.drawRoundedRect(cellRect(c.x(), c.y()).adjusted(1, 1, -1, -1), 5, 5);
    }

    if (m_wellFlash > 0)
        p.fillRect(well, withAlpha(QColor(200, 240, 255), m_wellFlash * 0.22));

    p.restore();
}

void TeqrisWidget::paintEffects(QPainter &p)
{
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Plus);
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
        const qreal alpha = k > 0.65 ? 1 - (k - 0.65) / 0.35 : 1.0;
        p.save();
        p.translate(t.pos + QPointF(0, -46 * k));
        p.scale(scale, scale);
        drawGlowText(p, QPointF(0, 0), t.text, displayFont(t.size, 2), withAlpha(t.color, alpha),
                     withAlpha(t.color, alpha), t.size * 0.5);
        p.restore();
    }
}

void TeqrisWidget::paintCountdown(QPainter &p)
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
    p.translate(WellX + WellW / 2, WellY + WellH / 2);
    p.scale(scale, scale);
    drawGlowText(p, QPointF(0, 0), text, displayFont(80, 4), withAlpha(TextBright, alpha),
                 withAlpha(accent(), alpha), 26);
    p.restore();
}

void TeqrisWidget::paintPause(QPainter &p)
{
    const QColor acc = accent();
    p.fillRect(QRectF(-2000, -2000, 5000, 5000), QColor(4, 2, 14, 190));

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

void TeqrisWidget::paintGameOver(QPainter &p)
{
    const qreal fade = std::min(1.0, m_stateTime / 450);
    const qreal slide = (1 - easeOutBack(fade)) * 40;
    p.fillRect(QRectF(-2000, -2000, 5000, 5000), QColor(4, 2, 14, int(200 * fade)));

    p.save();
    p.translate(0, slide);
    p.setOpacity(fade);

    const QColor red(255, 70, 120);
    const QRectF panel(200, 130, 420, 500);
    drawPanel(p, panel, QString(), red);
    drawNeonFrame(p, panel, withAlpha(red, 0.8), 10, 0.9);

    // Glitchy title
    const QFont titleFont = displayFont(44, 4);
    const QPointF titlePos(CenterX, panel.top() + 70);
    const qreal jitter = std::fmod(m_time, 2.2) < 0.12 ? rnd(-4, 4) : 0;
    drawGlowText(p, titlePos + QPointF(-3 + jitter, 0), QStringLiteral("GAME OVER"), titleFont,
                 withAlpha(QColor(0, 230, 255), 0.5), Qt::transparent, 0);
    drawGlowText(p, titlePos + QPointF(3 - jitter, 0), QStringLiteral("GAME OVER"), titleFont,
                 withAlpha(Magenta, 0.5), Qt::transparent, 0);
    drawGlowText(p, titlePos, QStringLiteral("GAME OVER"), titleFont, TextBright, red, 20);

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
        {QStringLiteral("LINES"), QString::number(m_game.lines()), TextBright},
        {QStringLiteral("LEVEL"), QString::number(m_game.level()), TextBright},
        {QStringLiteral("QUADS"), QString::number(m_game.quads()), TextBright},
        {QStringLiteral("BEST"), QString::number(m_best), Gold},
    };
    qreal y = panel.top() + 150;
    for (const Stat &s : stats) {
        drawText(p, QRectF(panel.left() + 50, y, 160, 34), s.label, displayFont(14, 4, QFont::Bold), TextDim,
                 Qt::AlignLeft | Qt::AlignVCenter);
        drawText(p, QRectF(panel.left() + 180, y, 190, 34), s.value, displayFont(22, 1), s.color,
                 Qt::AlignRight | Qt::AlignVCenter);
        p.setPen(QPen(QColor(255, 255, 255, 18), 1));
        p.drawLine(QPointF(panel.left() + 50, y + 38), QPointF(panel.right() - 50, y + 38));
        y += 44;
    }

    paintButton(p, QRectF(CenterX - 130, panel.top() + 372, 260, 48), QStringLiteral("RETRY"), m_selected == 0);
    paintButton(p, QRectF(CenterX - 130, panel.top() + 432, 260, 48), QStringLiteral("MAIN MENU"), m_selected == 1);
    p.restore();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void TeqrisWidget::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();

    switch (m_screen) {
    case Screen::Playing:
        if (!event->isAutoRepeat())
            handleGameKey(key);
        break;
    case Screen::Countdown:
        if (!event->isAutoRepeat() && (key == Qt::Key_Escape || key == Qt::Key_P)) {
            m_resumeScreen = Screen::Countdown;
            setScreen(Screen::Paused);
        }
        break;
    case Screen::Dying:
        break;
    case Screen::GameOver:
        // Don't let a hammered hard-drop key skip the results.
        if (m_stateTime > 800 && key != Qt::Key_Space)
            handleMenuKey(key);
        break;
    default:
        handleMenuKey(key);
        break;
    }
}

void TeqrisWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
        return;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
        m_leftHeld = false;
        if (m_dasDir < 0) {
            m_dasDir = m_rightHeld ? 1 : 0;
            m_dasTimer = m_arrTimer = 0;
        }
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
        m_rightHeld = false;
        if (m_dasDir > 0) {
            m_dasDir = m_leftHeld ? -1 : 0;
            m_dasTimer = m_arrTimer = 0;
        }
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        m_downHeld = false;
        break;
    default:
        break;
    }
}

void TeqrisWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = m_transform.inverted().map(event->position());
    for (int i = 0; i < m_hitRects.size(); ++i) {
        if (m_hitRects[i].contains(pos))
            m_selected = i;
    }
}

void TeqrisWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || (m_screen == Screen::GameOver && m_stateTime < 800))
        return;
    const QPointF pos = m_transform.inverted().map(event->position());
    const QStringList items = menuItems();
    for (int i = 0; i < m_hitRects.size() && i < items.size(); ++i) {
        if (!m_hitRects[i].contains(pos))
            continue;
        m_selected = i;
        if (items[i] == QLatin1String("level"))
            changeLevel(pos.x() < m_hitRects[i].center().x() ? -1 : 1);
        else
            activate(items[i]);
        return;
    }
}

void TeqrisWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    pauseIfRunning();
}

// Plain keys are the game's, not the window's: without this a single-key shortcut (Escape in a panel, …) could take
// them first. Ctrl / Alt combinations still reach the window (Ctrl+Alt+P closes the panel, Ctrl+J the terminal).
bool TeqrisWidget::event(QEvent *event)
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
bool TeqrisWidget::focusNextPrevChild(bool)
{
    return false;
}

void TeqrisWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_clock.restart();
    m_timer.start();
}

void TeqrisWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    pauseIfRunning();
    m_timer.stop();
}
