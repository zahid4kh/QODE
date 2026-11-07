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
// Windows (ProcessTreeWin.cpp): no watchdog. Each tracked process is created inside a Job Object of its own with
// KILL_ON_JOB_CLOSE, so Windows ends the tree when QODE goes away. stop() sends Ctrl+C to the tree's hidden console,
// kill() terminates the job.
namespace ProcessTree {

// Called first thing in main(): when this process is the watchdog, runs it and returns true (main then exits).
bool runIfWatchdog(int argc, char *argv[]);

// Unix: puts the process in its own process group (replacing the child process modifier) and registers that group
// from start to finish. Windows: creates it inside its own job (replacing the create-process-arguments modifier).
// Call it before start().
void track(QProcess *process);

// Asks the tree started as `pid` (a tracked process) to end: SIGTERM to its group on Unix, Ctrl+C to its console on
// Windows. Callers follow up with kill() after a timeout.
void stop(qint64 pid);

// stop() for a process whose input QODE does not use (a dev server, the auto-reload build). On Windows it also answers
// cmd's "Terminate batch job (Y/N)?" (a .bat / .cmd such as npm.cmd or gradlew.bat asks it after Ctrl+C): "Y", then the
// end of input, which ends the batch on a Windows in another language too (the prompt takes the end of input as No, and
// the batch then runs to its end). Unix: the same as stop(pid).
void stop(QProcess *process);

// Ends the tree started as `pid` at once: SIGKILL to its group on Unix, TerminateJobObject on Windows.
void kill(qint64 pid);

} // namespace ProcessTree
