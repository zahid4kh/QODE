#pragma once

#include <QString>
#include <QStringList>

class QProcess;

// Small OS services that have one implementation per OS: PlatformUnix.cpp (Linux, later macOS) and PlatformWin.cpp.
// No OS headers here; callers never #ifdef on the OS themselves.
namespace Platform {

// Called first in main(), with the watchdog check: true when this process was started as one of QODE's helpers (see
// restartSelf) and has done its job, so main returns at once. Unix: no helpers, always false.
bool runIfHelper(int argc, char *argv[]);

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

// Sets `process` up to run a command line through the system shell; start() it afterwards. Unix: /bin/sh -c. Windows:
// cmd.exe /d /s /c, with the line handed to cmd as it is (cmd's own quoting rules apply, and npm / npx resolve to their
// .cmd wrappers).
void setShellCommand(QProcess *process, const QString &command);

// Folders where per-user tools land that a desktop-launched QODE may lack on PATH, beyond the home-relative ones the
// callers add themselves (~/.local/bin, ~/.bun/bin, ~/.cargo/bin …, the same on every OS). Unix: none. Windows: npm's
// global folder, pnpm's, Node.js's, WinGet's links and Scoop's shims. Only folders that exist.
QStringList extraBinDirs();

// Starts QODE again once this process has ended; call it after the window closed (the project claim goes with the
// process). Unix: a shell that waits a second, then execs QODE. Windows: a helper that waits for this process to exit.
void restartSelf();

} // namespace Platform
