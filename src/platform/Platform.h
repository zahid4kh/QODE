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

// Sets `process` up to run `program` with `args`; start() it afterwards. Unix: exactly that. Windows: a bare name is
// looked up on PATH with PATHEXT (gradle -> gradle.bat), and a .bat / .cmd runs through cmd.exe with the arguments
// quoted for it (CreateProcess cannot start those itself). Arguments must not contain '%' on Windows (cmd expands it).
void setCommand(QProcess *process, const QString &program, const QStringList &args);

// The wrapper scripts projects ship next to their build file (`gradlew`, `mvnw`), by base name:
// - wrapperCommand: how a command line run in `dir` names it. Unix "./gradlew", or "sh gradlew" when the file lost its
//   exec bit; Windows ".\gradlew" (cmd and PowerShell both find gradlew.bat / mvnw.cmd from that). Empty when none.
// - wrapperFile: the file to start with setCommand (dir/gradlew, or dir/gradlew.bat on Windows); empty when there is
//   none or (Unix) it is not executable.
// - wrapperNeedsExecBit: Unix only, the script exists without its exec bit (a zip download, a checkout without modes).
QString wrapperCommand(const QString &dir, const QString &name);
QString wrapperFile(const QString &dir, const QString &name);
bool wrapperNeedsExecBit(const QString &dir, const QString &name);

// Python virtual environments: the folder that holds their programs ("bin" on Unix, "Scripts" on Windows), and one
// of them by base name (python, pip, ruff): venv/bin/<name> on Unix, venv/Scripts/<name>.exe on Windows. Empty when it
// is missing (or, on Unix, not executable).
QString venvBinDir();
QString venvProgram(const QString &venv, const QString &name);

// Whether files have an exec bit QODE should offer to set (chmod +x). Unix: true. Windows: false (a file runs by its
// extension, and QFileInfo::isExecutable only reports that).
bool hasExecBit();

// The git executable, empty when there is none. PATH first; on Windows also Git for Windows' usual install folders,
// for an install that did not add itself to PATH.
QString gitExecutable();

// Folders where per-user tools land that a desktop-launched QODE may lack on PATH, beyond the home-relative ones the
// callers add themselves (~/.local/bin, ~/.bun/bin, ~/.cargo/bin …, the same on every OS). Unix: none. Windows: npm's
// global folder, pnpm's, Node.js's, WinGet's links and Scoop's shims. Only folders that exist.
QStringList extraBinDirs();

// The current folder of process `pid`, empty when unknown. Unix: from /proc. Windows: always empty (another process's
// folder cannot be read reliably); the terminal gets it from its shell integration instead.
QString processCwd(qint64 pid);

// True while the terminal shell `pid` itself owns its terminal (it is at its prompt), not a program it started.
// Unix: the shell's process group is the terminal's foreground group (/proc). Windows: the shell has no child process.
bool shellInForeground(qint64 pid);

// The shells the terminal offers in its New Terminal menu: the name shown and the program. Unix: none (the terminal
// runs $SHELL, and the panel shows no menu). Windows: PowerShell 7, Windows PowerShell, Command Prompt, Git Bash and
// WSL (only with a distribution installed), those that are there.
struct Shell {
    QString name;
    QString program;
};
QList<Shell> terminalShells();

// Starts QODE again once this process has ended; call it after the window closed (the project claim goes with the
// process). Unix: a shell that waits a second, then execs QODE. Windows: a helper that waits for this process to exit.
void restartSelf();

} // namespace Platform
