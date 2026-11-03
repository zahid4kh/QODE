#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

// Desktop notifications through org.freedesktop.Notifications over D-Bus (built with QtDBus, QODE_HAS_DBUS). Without
// D-Bus nothing is shown; callers still call QApplication::alert. No tray icon (GNOME has none) and no notify-send
// (it may not be installed).
class DesktopNotify : public QObject
{
    Q_OBJECT
public:
    static DesktopNotify &instance();

    // `key` names the notification: another one with the same key replaces it instead of stacking.
    void notify(const QString &title, const QString &body, const QString &key);
    void close(const QString &key);

signals:
    void activated(const QString &key); // the user clicked the notification

private slots:
    void onActionInvoked(uint id, const QString &action);
    void onClosed(uint id, uint reason);

private:
    DesktopNotify();

    QHash<QString, uint> m_ids;  // key -> the server's id of the notification on screen
    QSet<QString> m_pendingClose; // closed before the server answered with its id
};
