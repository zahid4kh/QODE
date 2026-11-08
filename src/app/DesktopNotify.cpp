#include "DesktopNotify.h"

#ifdef QODE_HAS_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

namespace {
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kInterface = QStringLiteral("org.freedesktop.Notifications");
} // namespace
#elif defined(Q_OS_WIN)
#include <QApplication>
#include <QSystemTrayIcon>
#endif

DesktopNotify &DesktopNotify::instance()
{
    static DesktopNotify notify;
    return notify;
}

DesktopNotify::DesktopNotify()
{
#ifdef QODE_HAS_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kInterface, QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("NotificationClosed"), this, SLOT(onClosed(uint, uint)));
#endif
}

void DesktopNotify::notify(const QString &title, const QString &body, const QString &key)
{
#ifdef QODE_HAS_DBUS
    m_pendingClose.remove(key);
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("Notify"));
    // app_name, replaces_id, app_icon (the installed icon's name), summary, body (servers may read it as markup),
    // actions ("default" = a click on the notification), hints, expire_timeout (-1 = the server's default).
    const QVariantMap hints{{QStringLiteral("desktop-entry"), QStringLiteral("qode")}};
    msg << QStringLiteral("QODE") << m_ids.value(key, 0u) << QStringLiteral("qode") << title << body.toHtmlEscaped()
        << QStringList{QStringLiteral("default"), tr("Show")} << hints << int(-1);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, key](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isError())
            return;
        m_ids.insert(key, reply.value());
        if (m_pendingClose.remove(key))
            close(key);
    });
#elif defined(Q_OS_WIN)
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    if (!m_tray) {
        m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
        m_tray->setToolTip(QStringLiteral("QODE"));
        // A click on the toast, or on the icon while it is in the tray: the same as a click on a D-Bus notification.
        auto clicked = [this] {
            const QString shown = m_trayKey;
            hideTray();
            if (!shown.isEmpty())
                emit activated(shown);
        };
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, clicked);
        connect(m_tray, &QSystemTrayIcon::activated, this, [clicked](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                clicked();
        });
    }
    m_trayKey = key;
    m_tray->show();
    m_tray->showMessage(title, body, QApplication::windowIcon(), 10000);
#else
    Q_UNUSED(title)
    Q_UNUSED(body)
    Q_UNUSED(key)
#endif
}

void DesktopNotify::close(const QString &key)
{
#ifdef QODE_HAS_DBUS
    if (!m_ids.contains(key)) {
        m_pendingClose.insert(key); // in case its Notify call is still on its way
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("CloseNotification"));
    msg << m_ids.take(key);
    QDBusConnection::sessionBus().asyncCall(msg);
#elif defined(Q_OS_WIN)
    if (key == m_trayKey)
        hideTray();
#else
    Q_UNUSED(key)
#endif
}

void DesktopNotify::hideTray()
{
    // Removing the icon also takes its toast off the screen and out of the notification centre.
    m_trayKey.clear();
#ifdef Q_OS_WIN
    if (m_tray)
        m_tray->hide();
#endif
}

void DesktopNotify::onActionInvoked(uint id, const QString &action)
{
    if (action != QLatin1String("default"))
        return;
    for (auto it = m_ids.cbegin(); it != m_ids.cend(); ++it)
        if (it.value() == id) {
            emit activated(it.key());
            return;
        }
}

void DesktopNotify::onClosed(uint id, uint)
{
    for (auto it = m_ids.begin(); it != m_ids.end(); ++it)
        if (it.value() == id) {
            m_ids.erase(it);
            return;
        }
}
