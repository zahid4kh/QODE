#pragma once

#include <QString>

// Small OS services that have one implementation per OS: PlatformUnix.cpp (Linux, later macOS) and PlatformWin.cpp.
// No OS headers here; callers never #ifdef on the OS themselves.
namespace Platform {

// First thing in main(), before Qt starts. Unix: writing to a child process that already exited (git, the shell)
// fails with EPIPE instead of killing QODE with SIGPIPE. Windows: nothing to do.
void initProcess();

// How long the process ran before main() (loading and linking the libraries, static initialisers), in ms; -1 when
// unknown. Unix: from /proc (about 10 ms precise). Windows: from the process creation time.
qint64 msBeforeMain();

// Identifies the current OS user in names shared by every user of the machine (local socket / named pipe names).
// Unix: the numeric uid. Windows: the user's SID.
QString userKey();

// Replaces `link` with a symbolic link to `target` in one step (a half-written link is never visible). Returns false
// when it could not. Windows: always false for now (creating symlinks needs Developer Mode or admin rights; the
// installers that use it get a Windows layout in Phase 5 of the port).
bool replaceSymlink(const QString &target, const QString &link);

} // namespace Platform
