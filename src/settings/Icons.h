#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QAbstractButton;
class QAction;

// The bundled SVG icons are drawn in a single neutral stroke colour; these helpers recolour them
// so they stay readable in both themes.
namespace Icons {

// `disabled` is used for the Disabled icon mode (defaults to `color` at reduced opacity).
QIcon tinted(const QString &resource, const QColor &color, const QColor &disabled = QColor());
QPixmap pixmap(const QString &resource, const QColor &color, int size, qreal dpr = 2.0);

// Sets the icon now and again whenever the theme changes (in the theme's normal text colour).
void bind(QAction *action, const QString &resource);
void bind(QAbstractButton *button, const QString &resource);

} // namespace Icons
