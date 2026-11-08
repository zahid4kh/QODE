#include "filesystem/WatchGuard.h"
#include "platform/Platform.h"

#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>

namespace {

struct Owner {
    QPointer<QObject> object;
    std::function<void(const QString &)> release;
    std::function<void()> restore;
};

QList<Owner> &owners()
{
    static QList<Owner> list;
    return list;
}

int &depth() // nested scopes (a move of several folders) restore once, at the end
{
    static int d = 0;
    return d;
}

} // namespace

namespace WatchGuard {

void registerOwner(QObject *owner, std::function<void(const QString &)> release, std::function<void()> restore)
{
    owners().append({owner, std::move(release), std::move(restore)});
}

void registerWatcher(QFileSystemWatcher *watcher, std::function<void()> rearm)
{
    auto released = std::make_shared<bool>(false);
    registerOwner(
        watcher,
        [watcher, released](const QString &folder) {
            QStringList drop;
            for (const QString &p : watcher->files() + watcher->directories())
                if (Platform::samePath(p, folder) || Platform::isUnder(p, folder))
                    drop << p;
            if (!drop.isEmpty()) {
                watcher->removePaths(drop);
                *released = true;
            }
        },
        [released, rearm = std::move(rearm)] {
            if (!*released)
                return;
            *released = false;
            rearm();
        });
}

Scope::Scope(const QString &path)
{
    if (!Platform::watchesBlockRenames() || !QFileInfo(path).isDir())
        return;
    m_active = true;
    ++depth();
    for (const Owner &o : std::as_const(owners()))
        if (o.object && o.release)
            o.release(QFileInfo(path).absoluteFilePath());
}

Scope::~Scope()
{
    if (!m_active || --depth() > 0)
        return;
    for (const Owner &o : std::as_const(owners()))
        if (o.object && o.restore)
            o.restore();
}

} // namespace WatchGuard
