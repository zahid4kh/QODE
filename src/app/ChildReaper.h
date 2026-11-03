#pragma once

class QProcess;

// Stops the process groups QODE started (servers, the auto-reload build, the chat's commands) when QODE ends without
// shutting them down itself: a crash, `kill -9`, a SIGTERM. Each of those runs in its own process group (setsid), so
// nothing else would ever stop them. A small watchdog process (this same binary, started with --qode-reaper) reads the
// groups from a pipe only QODE holds; when QODE goes away in any way the pipe ends, and the watchdog sends the groups
// still registered SIGTERM, then SIGKILL to what is left after 3 s. Its own name is qode-reaper, and it ignores
// SIGTERM / SIGINT / SIGHUP, so `pkill -x qode` or a Ctrl+C in QODE's terminal never take it down first.
namespace ChildReaper {

// Called first thing in main(): when this process is the watchdog, runs it and returns true (main then exits).
bool runIfReaper(int argc, char *argv[]);

// Puts the process in its own process group (replacing the child process modifier) and registers that group from
// start to finish. Call it before start().
void track(QProcess *process);

} // namespace ChildReaper
