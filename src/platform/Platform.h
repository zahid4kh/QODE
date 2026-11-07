#pragma once

#include <QString>
#include <QStringList>

class QFileInfo;
class QProcess;

// Small OS services that have one implementation per OS: PlatformUnix.cpp (Linux, later macOS) and PlatformWin.cpp.
// No OS headers here; callers never #ifdef on the OS themselves.
namespace Platform {

// Called first in main(), with the watchdog check: true when this process was started as one of QODE's helpers (see
// restartSelf) and has done its job, so main returns at once. Unix: no helpers, always false.
bool runIfHelper(int argc, char *argv[]);

// First thing in main(), before Qt starts. Unix: writing to a child process that already exited (git, the shell)
// fails with EPIPE instead of killing QODE with SIGPIPE. Windows: PYTHONUTF8=1 and PYTHONIOENCODING=utf-8 for every
// child (unless set already), so Python reads and writes UTF-8, not the ANSI / OEM code page.
void initProcess();

// Text a child process wrote (pip, npm, Gradle, an installer). Unix: QString::fromLocal8Bit, as before. Windows: UTF-8
// when the bytes are valid UTF-8, else the system's ANSI code page (Java and many console tools write in it when their
// output is a pipe; QODE's own code page is UTF-8, so fromLocal8Bit would not do).
QString decodeOutput(const QByteArray &bytes);

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
// Whether QODE makes symbolic links (copying a link in the explorer). Unix: true. Windows: false (QFile::link writes a
// .lnk shortcut, and a real link needs Developer Mode or admin rights).
bool canMakeSymlinks();

// Paths. Inside QODE a path has Qt's form ('/', and a drive letter on Windows). Windows file names ignore case, so two
// paths that differ only in case are the same file there.
// - pathKey: the form to compare paths in or to key a map by. Unix: the path as it is. Windows: '/' and lower case.
// - samePath: pathKey(a) == pathKey(b).
// - isUnder: `path` lies below the folder `root` (not root itself), i.e. it starts with root + '/'; a root that already
//   ends in '/' (C:/, or / on Unix) is used as it is. Case-insensitive on Windows.
// - uriToPath: the local path of a file:// URI, "" for another scheme. Language servers on Windows send the drive in
//   either case and may percent-encode its colon (file:///c%3A/x); this always gives C:/x. Unix: QUrl::toLocalFile.
// - pathToUri: file:///… for a local path (QUrl::fromLocalFile), on every OS.
// - invalidFileName: why `name` (one file or folder name, no path) cannot be used here, "" when it can. Unix: always "".
//   Windows: the device names CON, PRN, AUX, NUL, COM1-9, LPT1-9 (also with an extension: nul.txt), the characters
//   < > : " | ? * \ and control characters, and a trailing dot or space (Windows drops them).
QString invalidFileName(const QString &name);
// How often a save tries again when it fails (50, 100, 150 ms apart). Unix: 0. Windows: 3 (a virus scanner or the
// indexer can hold a file that was just written for a moment).
int busyFileRetries();
// Whether watching a folder (or a file in it) keeps its parent folders from being renamed (WatchGuard). Unix: false.
// Windows: true.
bool watchesBlockRenames();
// The encoding a text file that is not UTF-8 is read in (TextEncoding). Unix: "Latin-1". Windows: "Windows-1252", the
// ANSI code page Windows tools wrote such files in (€ “ ” – — sit at 0x80-0x9F, where Latin-1 has control codes).
QString legacyEncoding();
// Whether the explorer should hide this entry when hidden files are not shown, besides dot-files. Unix: never.
// Windows: the Hidden attribute, and desktop.ini / Thumbs.db (Explorer's own files).
bool hiddenByOs(const QFileInfo &info);
QString pathKey(const QString &path);
bool samePath(const QString &a, const QString &b);
bool isUnder(const QString &path, const QString &root);
QString uriToPath(const QString &uri);
QString pathToUri(const QString &path);

// Sets `process` up to run an sh command line through the system shell; start() it afterwards. Unix: /bin/sh -c.
// Windows: cmd.exe /d /s /c, the line rewritten for cmd by ShellSyntax::translate (quotes, VAR=value, &&; a line it
// does not know goes on as it is), and npm / npx resolve to their .cmd wrappers.
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

// What the default run configurations (RunConfigDialog) name, for an sh line:
// - exeSuffix: what a compiler's `-o name` output is run as. Unix "", Windows ".exe".
// - pythonCommand: Unix "python3"; Windows "py" (the launcher) when there is one, else "python".
// - bashProgram: Unix "bash"; Windows Git Bash's bash.exe when Git has one (a bare bash may be WSL's), else "bash".
QString exeSuffix();
QString pythonCommand();
QString bashProgram();

// qmake projects (QmakeProject):
// - qmakeCandidates: qmake programs that are not on PATH, the best first. Unix: none. Windows: <drive>:\Qt\<version>\
//   <kit>\bin\qmake.exe (the Qt installer's layout), newest version first; MSVC kits first when Visual Studio's C++
//   tools are installed, MinGW kits first otherwise.
// - qmakeScopes: the scopes a .pro file's conditions see as true for this OS and that qmake. Unix: unix, linux,
//   linux-g++, linux-g++-64, gcc, posix. Windows: win32, windows, plus msvc / win32-msvc or mingw / win32-g++ / gcc.
// - qmakeRunCommand: an sh line that configures `proRel` (relative to the project) into <project>/build, builds it and
//   runs `exe` (relative to build/, or absolute). Unix: cd {project} && mkdir -p build && cd build && qmake ../x.pro &&
//   make -j$(nproc) && ./exe. Windows: a .bat written to QODE's cache that sets the compiler up (vcvars64.bat, or the
//   kit's MinGW) and Qt's bin\ on PATH, builds a release with jom / nmake / mingw32-make and runs exe.exe; the line runs
//   that file. Empty when that cannot be written.
QStringList qmakeCandidates();
QStringList qmakeScopes(const QString &qmake);
QString qmakeRunCommand(const QString &qmake, const QString &projectRoot, const QString &proRel, const QString &exe);

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
