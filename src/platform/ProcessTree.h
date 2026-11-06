#pragma once

#include <QtGlobal>

class QProcess;

// The process trees QODE starts (servers, the auto-reload build, the chat's commands, MCP servers): stopping a whole
// tree, and stopping what is left when QODE ends without shutting them down itself (a crash, `kill -9`).
//
// Unix (ProcessTreeUnix.cpp): each tracked process runs in its own process group (setsid), so nothing else would ever
// stop it. A small watchdog process (this same binary, started with --qode-reaper) reads the groups from a pipe only
// QODE holds; when QODE goes away in any way the pipe ends, and the watchdog sends the groups still registered SIGTERM,
// then SIGKILL to what is left after 3 s. Its own name is qode-reaper, and it ignores SIGTERM / SIGINT / SIGHUP, so
// `pkill -x qode` or a Ctrl+C in QODE's terminal never take it down first.
//
// Windows (ProcessTreeWin.cpp): no watchdog. stop() and kill() end the process and all its descendants. Not yet done:
// a Job Object per tracked process (killed by Windows when QODE dies) and a graceful stop (windows-port plan, Phase 2).
namespace ProcessTree {

// Called first thing in main(): when this process is the watchdog, runs it and returns true (main then exits).
bool runIfWatchdog(int argc, char *argv[]);

// Unix: puts the process in its own process group (replacing the child process modifier) and registers that group
// from start to finish. Call it before start().
void track(QProcess *process);

// Asks the tree started as `pid` (a tracked process) to end: SIGTERM to its group on Unix. Callers follow up with
// kill() after a timeout. Windows: ends the tree at once (no graceful stop yet).
void stop(qint64 pid);

// Ends the tree started as `pid` at once: SIGKILL to its group on Unix.
void kill(qint64 pid);

} // namespace ProcessTree
