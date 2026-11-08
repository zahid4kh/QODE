#pragma once

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QString>

class QPainter;

// The neon look shared by the game panel's games and its picker: palette, fonts and QPainter helpers. Game art keeps
// its own colours in every theme (like a picture), so these are constants, not Theme fields.
namespace GameArt {

inline const QColor Magenta(255, 60, 200);
inline const QColor Gold(255, 208, 84);
inline const QColor TextDim(146, 136, 190);
inline const QColor TextBright(236, 232, 255);
inline const QColor PanelFill(14, 10, 36, 215);

QColor withAlpha(QColor c, qreal a);
double rnd(double a, double b); // uniform in [a, b)
double easeOutBack(double t);

// Headings: Orbitron if it is installed, otherwise QODE's bundled UI font.
QFont displayFont(qreal px, qreal spacing = 0, QFont::Weight weight = QFont::Black);
// Numbers and small text: Share Tech Mono if it is installed, otherwise QODE's bundled editor font.
QFont bodyFont(qreal px, qreal spacing = 0, QFont::Weight weight = QFont::Bold);

// "42 MIN", "1H 05M" for the play time totals.
QString playTime(qint64 ms);

// Text whose vertical centre sits on anchor.y, with a soft neon halo.
void drawGlowText(QPainter &p, QPointF anchor, const QString &text, const QFont &font, const QBrush &fill, QColor glow,
                  qreal glowWidth, Qt::Alignment align = Qt::AlignHCenter);
void drawText(QPainter &p, const QRectF &rect, const QString &text, const QFont &font, QColor color,
              Qt::Alignment align = Qt::AlignCenter);
// A bevelled, glossy block (Teqris cells, the snake's food).
void drawBlock(QPainter &p, const QRectF &r, QColor c, qreal alpha = 1.0);
// A tetromino of the given type centred on center.
void drawPiece(QPainter &p, int type, QPointF center, qreal cell, qreal alpha = 1.0);
void drawNeonFrame(QPainter &p, const QRectF &r, QColor c, qreal radius, qreal intensity = 1.0);
void drawPanel(QPainter &p, const QRectF &r, const QString &title, QColor accent);
void drawTriangle(QPainter &p, QPointF center, qreal size, bool pointRight, QColor color);
// A keyboard key cap; returns its width.
qreal drawKeyCap(QPainter &p, QPointF topLeft, const QString &label, QColor accent);
// A pill-shaped menu button; the selected one glows and pulses with time (seconds).
void drawButton(QPainter &p, const QRectF &rect, const QString &text, bool selected, QColor accent, double time);

} // namespace GameArt
