#include "Icons.h"

#include "SettingsManager.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QHash>
#include <QPainter>
#include <QPixmap>

namespace Icons {

QPixmap pixmap(const QString &resource, const QColor &color, int size, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QString key = resource + QLatin1Char('|') + QString::number(color.rgba(), 16) + QLatin1Char('|') + QString::number(size)
                        + QLatin1Char('|') + QString::number(dpr);
    if (const auto it = cache.constFind(key); it != cache.constEnd())
        return it.value();
    const QIcon base(resource);
    QPixmap pm = base.pixmap(QSize(size, size) * dpr);
    if (pm.isNull())
        return pm;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), color);
    p.end();
    pm.setDevicePixelRatio(dpr);
    cache.insert(key, pm);
    return pm;
}

QIcon tinted(const QString &resource, const QColor &color, const QColor &disabled)
{
    QColor dis = disabled;
    if (!dis.isValid()) {
        dis = color;
        dis.setAlphaF(0.35);
    }
    QIcon icon;
    for (int size : {14, 16, 20, 24}) {
        icon.addPixmap(pixmap(resource, color, size), QIcon::Normal);
        icon.addPixmap(pixmap(resource, dis, size), QIcon::Disabled);
    }
    return icon;
}

namespace {
QColor themeFg() { return Theme::byName(SettingsManager::instance().theme()).editorFg; }
} // namespace

void bind(QAction *action, const QString &resource)
{
    action->setIcon(tinted(resource, themeFg()));
    QObject::connect(&SettingsManager::instance(), &SettingsManager::themeChanged, action,
                     [action, resource] { action->setIcon(tinted(resource, themeFg())); });
}

void bind(QAbstractButton *button, const QString &resource)
{
    button->setIcon(tinted(resource, themeFg()));
    QObject::connect(&SettingsManager::instance(), &SettingsManager::themeChanged, button,
                     [button, resource] { button->setIcon(tinted(resource, themeFg())); });
}

} // namespace Icons
