#pragma once

#include <QString>

#include <functional>

class QObject;

// Windows will not rename or move a folder while anything below it is watched: QFileSystemWatcher keeps a handle on
// each watched folder (and on the folder of each watched file), and an open handle below a folder pins it. QODE's
// watchers are the explorer's model (every folder that was expanded) and the editor's (every open file).
//
// Their owners register here; code that renames or moves a folder holds a WatchGuard::Scope around it, which asks
// every owner to let go of what it watches below that folder and to watch again afterwards. Unix needs none of this
// (Platform::watchesBlockRenames() is false), so a Scope does nothing there.
namespace WatchGuard {

// `release(folder)` stops watching what lies in or below `folder`; `restore()` watches what it should again (paths
// that moved are picked up by the owner's own rename handling). Both run on the GUI thread. Forgotten when `owner`
// is destroyed.
void registerOwner(QObject *owner, std::function<void(const QString &folder)> release, std::function<void()> restore);

class Scope
{
public:
    explicit Scope(const QString &path); // `path` may be a file: then nothing is released
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    bool m_active = false;
};

} // namespace WatchGuard
