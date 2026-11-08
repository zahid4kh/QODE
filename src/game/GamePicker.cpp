// The game panel's picker: a card per game.
#include "game/GamePicker.h"

#include "game/GameArt.h"
#include "game/GameStore.h"
#include "game/Tetromino.h"

#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

using namespace GameArt;

namespace {

constexpr qreal LogicalW = 820, LogicalH = 760;
constexpr qreal CenterX = LogicalW / 2;
constexpr qreal CardW = 330, CardH = 450, CardY = 182, CardGap = 40;

} // namespace

GamePicker::GamePicker(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_cards.append({QStringLiteral("teqris"), QStringLiteral("TEQRIS"), QStringLiteral("STACK  ·  ROTATE  ·  CLEAR"),
                    QColor(255, 80, 220)});
    m_cards.append({QStringLiteral("snake"), QStringLiteral("SNAKE"), QStringLiteral("STEER  ·  EAT  ·  GROW"),
                    QColor(60, 255, 170)});
    reloadStats();

    connect(&m_timer, &QTimer::timeout, this, &GamePicker::tick);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
}

QSize GamePicker::sizeHint() const
{
    return QSize(int(LogicalW), int(LogicalH));
}

QStringList GamePicker::gameIds()
{
    return {QStringLiteral("teqris"), QStringLiteral("snake")};
}

QString GamePicker::gameName(const QString &id)
{
    return id == QLatin1String("snake") ? QStringLiteral("SNAKE") : QStringLiteral("TEQRIS");
}

void GamePicker::select(const QString &id)
{
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i].id == id)
            m_selected = i;
    }
    update();
}

void GamePicker::reloadStats()
{
    for (Card &card : m_cards) {
        const GameStore store(card.id);
        card.best = store.bestScore();
        card.played = store.gamesPlayed();
        card.playMs = store.totalPlayMs();
    }
}

void GamePicker::tick()
{
    const double dt = std::min<double>(50.0, m_clock.restart());
    m_time += dt / 1000.0;
    for (int i = 0; i < m_cards.size(); ++i) {
        const double target = i == m_selected ? 1.0 : 0.0;
        m_cards[i].lift += (target - m_cards[i].lift) * std::min(1.0, dt * 0.014);
    }
    update();
}

void GamePicker::choose(int index)
{
    if (index < 0 || index >= m_cards.size())
        return;
    m_selected = index;
    emit gameChosen(m_cards[index].id);
}

void GamePicker::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    // Background: deep violet to teal, a soft haze behind the selected card's colour, drifting stars.
    const qreal w = width(), h = height();
    QLinearGradient bg(0, 0, w, h);
    bg.setColorAt(0, QColor(10, 5, 28));
    bg.setColorAt(0.55, QColor(14, 10, 38));
    bg.setColorAt(1, QColor(3, 16, 24));
    p.fillRect(rect(), bg);
    const QColor sel = m_cards[m_selected].accent;
    QRadialGradient haze(QPointF(w * (m_selected == 0 ? 0.3 : 0.7), h * 0.55), std::max(w, h) * 0.6);
    haze.setColorAt(0, withAlpha(sel, 0.16));
    haze.setColorAt(1, withAlpha(sel, 0));
    p.fillRect(rect(), haze);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 90; ++i) {
        // Deterministic positions: a cheap hash of the index.
        const qreal x = std::fmod(i * 0.618034 * 997, 1.0), y = std::fmod(i * 0.414214 * 613, 1.0);
        const qreal twinkle = 0.3 + 0.7 * (0.5 + 0.5 * std::sin(m_time * (0.6 + (i % 5) * 0.4) + i));
        p.setBrush(withAlpha(QColor(225, 230, 255), twinkle * 0.6));
        const qreal size = 0.5 + (i % 3) * 0.45;
        p.drawEllipse(QPointF(x * w, std::fmod(y * h + m_time * (4 + i % 4), h)), size, size);
    }

    const qreal scale = std::min(w / LogicalW, h / LogicalH);
    QTransform t;
    t.translate((w - LogicalW * scale) / 2, (h - LogicalH * scale) / 2);
    t.scale(scale, scale);
    m_transform = t;
    p.setTransform(t);
    m_hitRects.clear();

    // Title.
    const qreal bob = std::sin(m_time * 1.5) * 3;
    QLinearGradient titleFill(0, 60, 0, 120);
    titleFill.setColorAt(0, QColor(140, 250, 255));
    titleFill.setColorAt(0.5, Qt::white);
    titleFill.setColorAt(0.52, QColor(255, 140, 230));
    titleFill.setColorAt(1, QColor(150, 255, 190));
    drawGlowText(p, QPointF(CenterX, 88 + bob), QStringLiteral("ARCADE"), displayFont(64, 12), titleFill, sel, 18);
    drawText(p, QRectF(0, 134, LogicalW, 22), QStringLiteral("PICK A GAME WHILE QODE WORKS"), bodyFont(14, 6),
             withAlpha(TextBright, 0.65));

    const qreal total = m_cards.size() * CardW + (m_cards.size() - 1) * CardGap;
    for (int i = 0; i < m_cards.size(); ++i) {
        const QRectF r(CenterX - total / 2 + i * (CardW + CardGap), CardY, CardW, CardH);
        m_hitRects.append(r);
        paintCard(p, m_cards[i], r, i, i == m_selected);
    }

    drawText(p, QRectF(0, 690, LogicalW, 20),
             QStringLiteral("← →  CHOOSE     ENTER  PLAY     1 2  QUICK PICK     ESC  CLOSE"), bodyFont(13, 1),
             withAlpha(TextDim, 0.9));
    drawText(p, QRectF(0, 716, LogicalW, 20), QStringLiteral("SCORES ARE SAVED  ·  A GAME PAUSES WHEN YOU LEAVE IT"),
             bodyFont(11, 2), withAlpha(TextDim, 0.55));
}

void GamePicker::paintCard(QPainter &p, const Card &card, const QRectF &base, int index, bool selected)
{
    const QColor acc = card.accent;
    const qreal lift = card.lift;
    const QRectF r = base.translated(0, -10 * lift);

    p.save();
    p.setOpacity(0.6 + 0.4 * lift);

    // Shadow and body.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, int(60 + 60 * lift)));
    p.drawRoundedRect(r.translated(0, 8 + 8 * lift), 16, 16);
    QLinearGradient fill(r.topLeft(), r.bottomLeft());
    fill.setColorAt(0, QColor(22, 16, 52, 235));
    fill.setColorAt(1, QColor(10, 8, 28, 235));
    p.setBrush(fill);
    p.setPen(QPen(withAlpha(acc, 0.25 + 0.35 * lift), 1.5));
    p.drawRoundedRect(r, 16, 16);
    if (selected)
        drawNeonFrame(p, r, acc, 16, 0.6 + 0.4 * (0.5 + 0.5 * std::sin(m_time * 4)));

    // Preview.
    const QRectF area = r.adjusted(16, 16, -16, -(CardH - 236));
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(area, 10, 10);
    p.setClipPath(clip);
    QLinearGradient pv(area.topLeft(), area.bottomLeft());
    pv.setColorAt(0, QColor(6, 4, 20));
    pv.setColorAt(1, withAlpha(acc.darker(400), 1.0));
    p.fillRect(area, pv);
    if (card.id == QLatin1String("teqris"))
        paintTeqrisPreview(p, area, acc);
    else
        paintSnakePreview(p, area, acc);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(withAlpha(acc, 0.35), 1));
    p.drawRoundedRect(area, 10, 10);

    // Number badge.
    const QRectF badge(r.right() - 46, r.top() + 26, 26, 26);
    p.setPen(QPen(withAlpha(acc, 0.8), 1.2));
    p.setBrush(QColor(10, 6, 28, 220));
    p.drawRoundedRect(badge, 6, 6);
    drawText(p, badge, QString::number(index + 1), bodyFont(14), TextBright);

    // Name and tagline.
    drawGlowText(p, QPointF(r.center().x(), r.top() + 270), card.name, displayFont(36, 6), TextBright, acc,
                 selected ? 14 : 4);
    drawText(p, QRectF(r.left(), r.top() + 294, r.width(), 20), card.tagline, bodyFont(12, 3), withAlpha(acc, 0.85));

    // Stats.
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.drawLine(QPointF(r.left() + 30, r.top() + 326), QPointF(r.right() - 30, r.top() + 326));
    drawText(p, QRectF(r.left() + 30, r.top() + 336, 120, 18), QStringLiteral("HIGH SCORE"),
             displayFont(10, 3, QFont::Bold), withAlpha(Gold, 0.8), Qt::AlignLeft | Qt::AlignVCenter);
    drawText(p, QRectF(r.left() + 30, r.top() + 354, r.width() - 60, 32),
             card.best > 0 ? QString::number(card.best) : QStringLiteral("—"), displayFont(24, 2), Gold,
             Qt::AlignLeft | Qt::AlignVCenter);
    drawText(p, QRectF(r.left() + 30, r.top() + 336, r.width() - 60, 18), QStringLiteral("PLAYED"),
             displayFont(10, 3, QFont::Bold), withAlpha(TextDim, 0.9), Qt::AlignRight | Qt::AlignVCenter);
    drawText(p, QRectF(r.left() + 30, r.top() + 354, r.width() - 60, 32),
             card.played > 0 ? QStringLiteral("%1  ·  %2").arg(card.played).arg(playTime(card.playMs))
                             : QStringLiteral("NEW"),
             bodyFont(15, 1), TextBright, Qt::AlignRight | Qt::AlignVCenter);

    // Play button.
    const QRectF play(r.left() + 40, r.bottom() - 58, r.width() - 80, 42);
    drawButton(p, play, selected ? QStringLiteral("PLAY") : QStringLiteral("SELECT"), selected, acc, m_time);
    p.restore();
}

void GamePicker::paintTeqrisPreview(QPainter &p, const QRectF &area, QColor accent)
{
    // A small well with a stack and a piece that keeps falling into the gap.
    const qreal cell = 20;
    const int cols = 8, rows = 10;
    const QRectF well(area.center().x() - cols * cell / 2, area.bottom() - rows * cell - 6, cols * cell, rows * cell);
    p.setPen(QPen(withAlpha(accent, 0.5), 1.5));
    p.setBrush(QColor(4, 3, 14, 200));
    p.drawRoundedRect(well.adjusted(-3, -3, 3, 3), 4, 4);

    // Rows from the bottom; -1 = empty. The gap in the bottom rows is where the I piece lands.
    static const int stack[4][8] = {{2, 2, 5, 5, 5, -1, 6, 6},
                                    {1, 1, 4, 5, 3, -1, 6, 0},
                                    {1, 1, 4, 4, 3, -1, 3, 0},
                                    {-1, -1, -1, 4, 3, -1, -1, 0}};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < cols; ++x) {
            if (stack[y][x] >= 0)
                drawBlock(p, QRectF(well.left() + x * cell, well.bottom() - (y + 1) * cell, cell, cell),
                          Tetromino::color(stack[y][x]));
        }
    }
    // The I piece falls down column 5 and the row flashes as it lands.
    const double cycle = std::fmod(m_time, 2.6);
    const double fall = std::min(1.0, cycle / 1.6);
    const qreal topY = well.top() - 4 * cell + fall * (rows) * cell;
    p.save();
    p.setClipRect(well.adjusted(0, -cell * 4, 0, 0));
    if (cycle < 1.6) {
        for (int i = 0; i < 4; ++i)
            drawBlock(p, QRectF(well.left() + 5 * cell, topY + i * cell, cell, cell), Tetromino::color(Tetromino::I));
    } else {
        const qreal k = (cycle - 1.6) / 1.0;
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(Qt::white, 0.6 * (1 - k)));
        p.drawRect(QRectF(well.left(), well.bottom() - 3 * cell, well.width(), 3 * cell));
        for (int i = 0; i < 4; ++i)
            drawBlock(p, QRectF(well.left() + 5 * cell, well.bottom() - (i + 1) * cell, cell, cell),
                      Tetromino::color(Tetromino::I), 1 - k);
    }
    p.restore();

    // Floating pieces on the sides.
    for (int i = 0; i < 4; ++i) {
        const qreal x = i < 2 ? area.left() + 34 : area.right() - 34;
        const qreal y = area.top() + std::fmod(30 + i * 70 + m_time * 18, area.height() + 40) - 20;
        p.save();
        p.translate(x, y);
        p.rotate(m_time * (i % 2 ? 25 : -20) + i * 50);
        drawPiece(p, (i * 3 + 1) % Tetromino::Count, QPointF(0, 0), 11, 0.35);
        p.restore();
    }
}

void GamePicker::paintSnakePreview(QPainter &p, const QRectF &area, QColor accent)
{
    // Faint grid.
    p.setPen(QPen(QColor(255, 255, 255, 10), 1));
    for (qreal x = area.left(); x < area.right(); x += 18)
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    for (qreal y = area.top(); y < area.bottom(); y += 18)
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));

    // A snake tracing a figure eight.
    const QPointF c = area.center();
    const qreal ax = area.width() * 0.36, ay = area.height() * 0.32;
    auto at = [&](double s) { return c + QPointF(std::sin(s) * ax, std::sin(2 * s) * ay); };
    const double head = m_time * 1.3;
    const int segments = 26;

    // Food sits on the path just ahead and is "eaten" each lap.
    const double lap = 2 * 3.14159265358979323846;
    const double foodS = std::floor(head / lap + 0.5) * lap + lap * 0.5;
    const QPointF food = at(foodS);
    const qreal pulse = 1 + 0.1 * std::sin(m_time * 6);
    QRadialGradient glow(food, 24);
    glow.setColorAt(0, QColor(255, 64, 120, 120));
    glow.setColorAt(1, QColor(255, 64, 120, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(food, 24, 24);
    p.setBrush(QColor(255, 64, 120));
    p.drawEllipse(food, 7 * pulse, 7 * pulse);

    QVector<QPointF> pts;
    for (int i = 0; i <= segments; ++i)
        pts.append(at(head - i * 0.11));
    const qreal hue = accent.hsvHueF();
    for (int i = segments; i >= 1; --i) {
        const qreal along = qreal(i) / segments;
        const QColor col = QColor::fromHsvF(std::fmod(hue + along * 0.2, 1.0), 0.62 + 0.2 * along, 1.0 - 0.25 * along);
        p.setPen(QPen(col, 14 * (1 - 0.4 * along * along), Qt::SolidLine, Qt::RoundCap));
        p.drawLine(pts[i], pts[i - 1]);
    }
    // Head and eyes.
    QPointF dir = pts[0] - pts[1];
    const qreal len = std::hypot(dir.x(), dir.y());
    dir = len > 0 ? dir / len : QPointF(1, 0);
    const QPointF perp(-dir.y(), dir.x());
    p.setPen(Qt::NoPen);
    p.setBrush(accent.lighter(115));
    p.drawEllipse(pts[0], 9, 9);
    for (int side : {-1, 1}) {
        const QPointF eye = pts[0] + dir * 2.5 + perp * side * 4;
        p.setBrush(Qt::white);
        p.drawEllipse(eye, 2.6, 2.6);
        p.setBrush(QColor(15, 10, 30));
        p.drawEllipse(eye + dir, 1.3, 1.3);
    }
}

void GamePicker::keyPressEvent(QKeyEvent *event)
{
    const int n = m_cards.size();
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
    case Qt::Key_Up:
    case Qt::Key_W:
        m_selected = (m_selected + n - 1) % n;
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
    case Qt::Key_Down:
    case Qt::Key_S:
    case Qt::Key_Tab:
        m_selected = (m_selected + 1) % n;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (!event->isAutoRepeat())
            choose(m_selected);
        break;
    case Qt::Key_1:
    case Qt::Key_2:
        if (!event->isAutoRepeat())
            choose(event->key() - Qt::Key_1);
        break;
    case Qt::Key_Escape:
        if (!event->isAutoRepeat())
            emit closeRequested();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    update();
}

void GamePicker::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = m_transform.inverted().map(event->position());
    bool over = false;
    for (int i = 0; i < m_hitRects.size(); ++i) {
        if (m_hitRects[i].contains(pos)) {
            m_selected = i;
            over = true;
        }
    }
    if (over != m_hoverCard) {
        m_hoverCard = over;
        if (over)
            setCursor(Qt::PointingHandCursor);
        else
            unsetCursor();
    }
}

void GamePicker::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = m_transform.inverted().map(event->position());
    for (int i = 0; i < m_hitRects.size(); ++i) {
        if (m_hitRects[i].contains(pos)) {
            choose(i);
            return;
        }
    }
}

void GamePicker::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_hoverCard = false;
    unsetCursor();
}

// Plain keys belong to the picker, not to window shortcuts (see TeqrisWidget::event).
bool GamePicker::event(QEvent *event)
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

bool GamePicker::focusNextPrevChild(bool)
{
    return false;
}

void GamePicker::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    reloadStats(); // a game may have just been finished
    m_clock.restart();
    m_timer.start();
}

void GamePicker::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer.stop();
}
