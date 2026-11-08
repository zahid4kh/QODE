// Neon drawing helpers shared by the game panel's games and its picker.
#include "game/GameArt.h"

#include "game/Tetromino.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QStringList>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <random>

namespace GameArt {

QColor withAlpha(QColor c, qreal a)
{
    c.setAlphaF(std::clamp(a, 0.0, 1.0));
    return c;
}

double rnd(double a, double b)
{
    static std::mt19937 rng(std::random_device{}());
    return std::uniform_real_distribution<double>(a, b)(rng);
}

double easeOutBack(double t)
{
    const double c = 1.70158;
    return 1 + (c + 1) * std::pow(t - 1, 3) + c * std::pow(t - 1, 2);
}

static QFont makeFont(const QStringList &families, qreal px, QFont::Weight weight, qreal spacing)
{
    QFont f;
    f.setFamilies(families);
    f.setPixelSize(std::max(1, int(px)));
    f.setWeight(weight);
    if (spacing != 0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

// Headings: Orbitron if it is installed, otherwise QODE's bundled UI font.
QFont displayFont(qreal px, qreal spacing, QFont::Weight weight)
{
    return makeFont({QStringLiteral("Orbitron"), QStringLiteral("Audiowide"), QStringLiteral("Plus Jakarta Sans"),
                     QStringLiteral("DejaVu Sans")},
                    px, weight, spacing);
}

// Numbers and small text: Share Tech Mono if it is installed, otherwise QODE's bundled editor font.
QFont bodyFont(qreal px, qreal spacing, QFont::Weight weight)
{
    return makeFont({QStringLiteral("Share Tech Mono"), QStringLiteral("JetBrains Mono"),
                     QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Monospace")},
                    px, weight, spacing);
}

// "42 MIN", "1H 05M" for the play time totals.
QString playTime(qint64 ms)
{
    const qint64 minutes = ms / 60000;
    if (minutes < 60)
        return QStringLiteral("%1 MIN").arg(minutes);
    return QStringLiteral("%1H %2M").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

// Text whose vertical centre sits on anchor.y, with a soft neon halo.
void drawGlowText(QPainter &p, QPointF anchor, const QString &text, const QFont &font, const QBrush &fill,
                  QColor glow, qreal glowWidth, Qt::Alignment align)
{
    const QFontMetricsF fm(font);
    const qreal w = fm.horizontalAdvance(text);
    qreal x = anchor.x();
    if (align & Qt::AlignHCenter)
        x -= w / 2;
    else if (align & Qt::AlignRight)
        x -= w;
    const qreal y = anchor.y() + (fm.ascent() - fm.descent()) / 2;

    QPainterPath path;
    path.addText(x, y, font, text);

    p.save();
    p.setBrush(Qt::NoBrush);
    if (glowWidth > 0) {
        const qreal baseAlpha = glow.alphaF();
        for (int i = 3; i >= 1; --i) {
            p.setPen(QPen(withAlpha(glow, baseAlpha * 0.16 * (4 - i)), glowWidth * i / 3.0, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        }
    }
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(path);
    p.restore();
}

void drawText(QPainter &p, const QRectF &rect, const QString &text, const QFont &font, QColor color,
              Qt::Alignment align)
{
    p.setFont(font);
    p.setPen(color);
    p.drawText(rect, align, text);
}

// A bevelled, glossy block.
void drawBlock(QPainter &p, const QRectF &r, QColor c, qreal alpha)
{
    const QRectF b = r.adjusted(1, 1, -1, -1);
    const qreal w = b.width();
    const qreal radius = w * 0.16;

    QLinearGradient outer(b.topLeft(), b.bottomRight());
    outer.setColorAt(0, withAlpha(c.lighter(150), alpha));
    outer.setColorAt(0.5, withAlpha(c, alpha));
    outer.setColorAt(1, withAlpha(c.darker(170), alpha));
    p.setPen(QPen(withAlpha(c.darker(230), alpha), 1));
    p.setBrush(outer);
    p.drawRoundedRect(b, radius, radius);

    const QRectF face = b.adjusted(w * 0.17, w * 0.17, -w * 0.17, -w * 0.17);
    QLinearGradient inner(face.topLeft(), face.bottomLeft());
    inner.setColorAt(0, withAlpha(c.lighter(125), alpha));
    inner.setColorAt(1, withAlpha(c.darker(115), alpha));
    p.setPen(Qt::NoPen);
    p.setBrush(inner);
    p.drawRoundedRect(face, radius * 0.6, radius * 0.6);

    p.setBrush(withAlpha(Qt::white, 0.45 * alpha));
    p.drawRoundedRect(QRectF(b.left() + w * 0.12, b.top() + w * 0.09, w * 0.32, w * 0.09), w * 0.04, w * 0.04);
}

void drawPiece(QPainter &p, int type, QPointF center, qreal cell, qreal alpha)
{
    if (type < 0)
        return;
    const auto cells = Tetromino::cells(type, 0);
    int minX = 9, maxX = -9, minY = 9, maxY = -9;
    for (const QPoint &c : cells) {
        minX = std::min(minX, c.x());
        maxX = std::max(maxX, c.x());
        minY = std::min(minY, c.y());
        maxY = std::max(maxY, c.y());
    }
    const QPointF origin = center - QPointF((maxX - minX + 1) * cell / 2, (maxY - minY + 1) * cell / 2);
    for (const QPoint &c : cells)
        drawBlock(p, QRectF(origin.x() + (c.x() - minX) * cell, origin.y() + (c.y() - minY) * cell, cell, cell),
                  Tetromino::color(type), alpha);
}

void drawNeonFrame(QPainter &p, const QRectF &r, QColor c, qreal radius, qreal intensity)
{
    p.setBrush(Qt::NoBrush);
    for (int i = 4; i >= 1; --i) {
        p.setPen(QPen(withAlpha(c, 0.055 * intensity * (5 - i)), i * 4.0));
        p.drawRoundedRect(r, radius, radius);
    }
    p.setPen(QPen(c.lighter(140), 2));
    p.drawRoundedRect(r, radius, radius);
}

void drawPanel(QPainter &p, const QRectF &r, const QString &title, QColor accent)
{
    p.setPen(QPen(withAlpha(accent, 0.35), 1.5));
    p.setBrush(PanelFill);
    p.drawRoundedRect(r, 10, 10);
    if (title.isEmpty())
        return;
    drawText(p, QRectF(r.x(), r.y() + 8, r.width(), 22), title, displayFont(13, 4, QFont::Bold), accent);
    QLinearGradient line(r.left() + 20, 0, r.right() - 20, 0);
    line.setColorAt(0, withAlpha(accent, 0));
    line.setColorAt(0.5, withAlpha(accent, 0.6));
    line.setColorAt(1, withAlpha(accent, 0));
    p.setPen(QPen(QBrush(line), 1));
    p.drawLine(QPointF(r.left() + 20, r.top() + 36), QPointF(r.right() - 20, r.top() + 36));
}

void drawTriangle(QPainter &p, QPointF center, qreal size, bool pointRight, QColor color)
{
    const qreal d = pointRight ? 1 : -1;
    QPainterPath path;
    path.moveTo(center + QPointF(d * size, 0));
    path.lineTo(center + QPointF(-d * size * 0.7, -size));
    path.lineTo(center + QPointF(-d * size * 0.7, size));
    path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(path);
}

// A keyboard key cap, returns its width.
qreal drawKeyCap(QPainter &p, QPointF topLeft, const QString &label, QColor accent)
{
    const QFont font = bodyFont(15, 1);
    const qreal w = std::max<qreal>(34, QFontMetricsF(font).horizontalAdvance(label) + 20);
    const QRectF r(topLeft, QSizeF(w, 32));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 120));
    p.drawRoundedRect(r.translated(0, 3), 6, 6);
    p.setPen(QPen(withAlpha(accent, 0.6), 1.2));
    p.setBrush(QColor(40, 32, 80));
    p.drawRoundedRect(r, 6, 6);
    drawText(p, r, label, font, TextBright);
    return w;
}

void drawButton(QPainter &p, const QRectF &rect, const QString &text, bool selected, QColor accent, double time)
{
    if (selected) {
        const qreal pulse = 0.75 + 0.25 * std::sin(time * 5);
        QLinearGradient fill(rect.topLeft(), rect.topRight());
        fill.setColorAt(0, withAlpha(accent, 0.55));
        fill.setColorAt(1, withAlpha(Magenta, 0.55));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
        drawNeonFrame(p, rect, accent, rect.height() / 2, pulse);
        drawTriangle(p, QPointF(rect.left() + 24, rect.center().y()), 7, true, withAlpha(Qt::white, pulse));
        drawTriangle(p, QPointF(rect.right() - 24, rect.center().y()), 7, false, withAlpha(Qt::white, pulse));
        drawGlowText(p, rect.center(), text, displayFont(19, 3), Qt::white, accent, 8);
    } else {
        p.setPen(QPen(withAlpha(accent, 0.25), 1.2));
        p.setBrush(QColor(255, 255, 255, 10));
        p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
        drawText(p, rect, text, displayFont(17, 3, QFont::Bold), TextDim);
    }
}

} // namespace GameArt
