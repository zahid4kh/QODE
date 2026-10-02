#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

class QLocalServer;

// Tells QODE windows (separate processes) which projects and files each one has open, so the same
// project or file is never edited in two windows. A claim is a listening local socket (a small file in
// the temp dir, removed on close); a socket left by a crashed process refuses connections and is replaced.
class InstanceRegistry : public QObject
{
    Q_OBJECT
public:
    enum Kind { Project, File };

    static InstanceRegistry &instance();

    // True if another QODE process has claimed it (a path this process claimed is never "elsewhere").
    bool isHeldElsewhere(Kind kind, const QString &path) const;
    void claim(Kind kind, const QString &path); // idempotent
    void release(Kind kind, const QString &path);
    // Make this process's claims of `kind` exactly `paths`.
    void setClaims(Kind kind, const QStringList &paths);

private:
    InstanceRegistry() = default;
    static QString nameFor(Kind kind, const QString &path);

    QHash<QString, QLocalServer *> m_servers; // socket name -> our claim
    QHash<QString, QString> m_paths;          // socket name -> normalized path
};
