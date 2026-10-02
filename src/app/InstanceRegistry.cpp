#include "InstanceRegistry.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>

#include <unistd.h>

namespace {
QString normalized(const QString &path)
{
    const QFileInfo fi(path);
    const QString canon = fi.canonicalFilePath();
    return canon.isEmpty() ? fi.absoluteFilePath() : canon;
}
} // namespace

InstanceRegistry &InstanceRegistry::instance()
{
    static InstanceRegistry r;
    return r;
}

QString InstanceRegistry::nameFor(Kind kind, const QString &path)
{
    const QByteArray hash = QCryptographicHash::hash(normalized(path).toUtf8(), QCryptographicHash::Sha1).toHex().left(20);
    // The abstract namespace is shared by every user on the machine, so the uid is part of the name.
    return QStringLiteral("qode-%1-%2-%3").arg(getuid()).arg(kind == Project ? QLatin1String("p") : QLatin1String("f"), QString::fromLatin1(hash));
}

bool InstanceRegistry::isHeldElsewhere(Kind kind, const QString &path) const
{
    const QString name = nameFor(kind, path);
    if (m_servers.contains(name))
        return false;
    // Connecting to a unix socket completes (or fails) immediately, so this never waits.
    QLocalSocket probe;
    probe.connectToServer(name);
    const bool held = probe.waitForConnected(50);
    probe.abort();
    return held;
}

void InstanceRegistry::claim(Kind kind, const QString &path)
{
    const QString name = nameFor(kind, path);
    if (m_servers.contains(name))
        return;
    auto *server = new QLocalServer(this);
    // A crashed process leaves its socket file behind; nobody answers on it, so it is safe to remove.
    if (!server->listen(name)) {
        QLocalServer::removeServer(name);
        if (!server->listen(name)) {
            delete server; // the claim is advisory, so carry on without it
            return;
        }
    }
    // Probes only need to connect; drop them straight away.
    connect(server, &QLocalServer::newConnection, server, [server] {
        while (QLocalSocket *s = server->nextPendingConnection())
            s->deleteLater();
    });
    m_servers.insert(name, server);
    m_paths.insert(name, normalized(path));
}

void InstanceRegistry::release(Kind kind, const QString &path)
{
    const QString name = nameFor(kind, path);
    if (QLocalServer *s = m_servers.take(name)) {
        s->close();
        s->deleteLater();
    }
    m_paths.remove(name);
}

void InstanceRegistry::setClaims(Kind kind, const QStringList &paths)
{
    const QString marker = kind == Project ? QStringLiteral("-p-") : QStringLiteral("-f-");
    QSet<QString> wanted;
    for (const QString &p : paths) {
        wanted.insert(nameFor(kind, p));
        claim(kind, p);
    }
    const QStringList names = m_servers.keys();
    for (const QString &name : names)
        if (name.contains(marker) && !wanted.contains(name)) {
            QLocalServer *s = m_servers.take(name);
            s->close();
            s->deleteLater();
            m_paths.remove(name);
        }
}
