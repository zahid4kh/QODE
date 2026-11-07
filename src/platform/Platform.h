#pragma once

#include <QList>
#include <QMap>
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
// child (unless set already), so Python reads and writes UTF-8, not the ANSI / OEM code page. Also asks Qt's Windows
// plugin to tell AltGr from Ctrl+Alt (QT_QPA_PLATFORM=windows:altgr, unless the user set that variable), see initGui.
void initProcess();

// Right after the QApplication is created. Unix: nothing. Windows: removes the QT_QPA_PLATFORM that initProcess set
// (children must not inherit it) and makes AltGr typing look as it does on Linux: Qt reports AltGr as
// GroupSwitchModifier (not Ctrl+Alt), so Ctrl+Alt shortcuts no longer swallow the characters AltGr types (German €,
// Polish ę ć ś), and once shortcuts are matched an app-wide filter drops that modifier from key presses, so the
// editor reads `{` typed with AltGr+7 as plain typing (auto-pairing, completion).
void initGui();

// QODE's theme is dark (or light); called before the theme's palette is set. Unix: nothing (the window manager draws
// the title bars). Windows: Qt's colour scheme follows QODE's theme instead of the Windows setting
// (QStyleHints::setColorScheme, Qt 6.8), so a dark theme gets dark title bars on every window and dialog, also when
// Windows is set to light, and a light one light title bars.
void setDarkTheme(bool dark);

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
// when it could not. Windows: always false (creating symlinks needs Developer Mode or admin rights): use the functions
// below, which have a Windows form.
bool replaceSymlink(const QString &target, const QString &link);

// Deletes a folder and everything in it (or a file); true when nothing is left. Unix: QDir::removeRecursively, which
// never follows a symlink. Windows: Qt's removeRecursively goes inside a junction and deletes what it points at (pnpm's
// node_modules and Oracle's Java\latest are junctions), so a junction or folder symlink is removed as a link only;
// read-only files are deleted too.
bool removeTree(const QString &path);

// The remove dialogs (LspRemoveDialog) show and run one command per path: a folder and everything in it, or (onlyFile)
// one file or link. removeCommandText: the line shown (Unix: with `shownPath`, ~/… ; Windows: the full `path`, since
// cmd has no ~). setRemoveCommand: sets `process` up to run it on `path`; it exits non-zero when the path is still
// there. Unix: rm -rf -- path / rm -- path. Windows: cmd's rmdir /s /q (it removes a junction as a link and deletes
// read-only files) / del /f /q.
QString removeCommandText(const QString &path, const QString &shownPath, bool onlyFile);
void setRemoveCommand(QProcess *process, const QString &path, bool onlyFile);

// Managed installs (the Kotlin LSP and jdtls installers) keep <root>/<version>/ and point <root>/current at the
// active one. Unix: a relative symlink (replaceSymlink). Windows: a directory junction, which needs no admin rights
// and can be swapped while the old version still runs. currentVersion: the folder name `current` points at, "" when
// there is none.
bool setCurrentVersion(const QString &root, const QString &version);
QString currentVersion(const QString &root);

// A command in a PATH folder that runs `target` (the Kotlin LSP installer's ~/.local/bin/kotlin-lsp):
// - commandLinkFile: the file name it gets. Unix: `link`. Windows: `link`.cmd.
// - writeCommandLink: Unix a symlink to target (replaceSymlink). Windows a one-line batch file: @"target" %*
// - commandLinkTarget: what an existing one runs, "" when the file is not such a link (someone else's file).
QString commandLinkFile(const QString &link);
bool writeCommandLink(const QString &target, const QString &file);
QString commandLinkTarget(const QString &file);

// Sets `process` up to unpack `archive` into the existing folder `dir`. Unix: tar -xzf (a .tar.gz). Windows:
// System32\tar.exe -xf, which reads .tar.gz and .zip (a Git tar earlier on PATH would read "C:" as a host name).
void setUnpackCommand(QProcess *process, const QString &archive, const QString &dir);

// The OS word in per-OS download and folder names: jdtls' config_<tag>, the Kotlin LSP's archive. Unix: "linux".
// Windows: "win".
QString osTag();
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
// looked up with PATHEXT (gradle -> gradle.bat, npx -> npx.cmd) on the PATH of the process environment already set on
// `process`, else on QODE's own; a .bat / .cmd runs through cmd.exe with the arguments
// quoted for it (CreateProcess cannot start those itself). Arguments must not contain '%' on Windows (cmd expands it).
void setCommand(QProcess *process, const QString &program, const QStringList &args);

// The wrapper scripts projects ship next to their build file (`gradlew`, `mvnw`), by base name:
// - wrapperCommand: how a command line run in `dir` names it. Unix "./gradlew", or "sh gradlew" when the file lost its
//   exec bit; Windows ".\gradlew.bat" / ".\mvnw.cmd" (the file's own name; cmd and PowerShell run it). Empty when none.
//   Code that reads a command line back accepts every form (./gradlew, .\gradlew, .\gradlew.bat, sh gradlew).
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

// Ends a short-lived tool started with setCommand and everything it started. Windows: a .bat / .cmd runs inside cmd,
// and killing only cmd would leave its java / node running; the descendants are ended first. Unix: process->kill().
// (Long-running servers use ProcessTree instead.)
void killTree(QProcess *process);

// Whether files have an exec bit QODE should offer to set (chmod +x). Unix: true. Windows: false (a file runs by its
// extension, and QFileInfo::isExecutable only reports that).
bool hasExecBit();

// What the default run configurations (RunConfigDialog) name, for an sh line:
// - exeSuffix: what a compiler's `-o name` output is run as. Unix "", Windows ".exe".
// - pythonCommand: Unix "python3"; Windows "py" (the launcher) when there is one, else "python".
// - bashProgram: Unix "bash"; Windows Git Bash's bash.exe when Git has one (a bare bash may be WSL's), else "bash".
QString exeSuffix();
QString pythonCommand();
// The Python 3 interpreter QODE creates virtual environments with (the Python language tools, the package manager),
// empty when there is none. Unix: python3 / python on PATH, then /usr/bin, /usr/local/bin, /bin. Windows: what the py
// launcher runs (the python.org installer's or the Python install manager's), then a python.exe on PATH that is not
// the Microsoft Store stub (WindowsApps), then the installers' folders (newest first).
QString pythonInterpreter();
QString bashProgram();
// Git for Windows' bash.exe (what the Claude Code CLI needs on Windows), empty when there is none. Unix: always empty.
QString gitBashExecutable();

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

// Whether Visual Studio's C++ compiler (cl, set up by vcvars64.bat) is installed. Unix: false.
bool hasMsvc();

// Values the project templates' Windows scripts use as {{key}} (ProjectTemplates adds them to every template's
// values), computed when asked. Unix: none. Windows:
// - cCompiler / cxxCompiler: the default compiler choice: msvc when Visual Studio's C++ tools are installed, else gcc /
//   g++ or clang / clang++ when on PATH, else msvc.
// - qmake: the Qt kit's qmake.exe QODE would use (PATH, else the Qt installer's newest kit), for a batch file (native
//   separators, % doubled); qtKit: "msvc" or "mingw" (which compiler that kit needs). Empty when no Qt is found.
QMap<QString, QString> templateValues();
QStringList qmakeScopes(const QString &qmake);
QString qmakeRunCommand(const QString &qmake, const QString &projectRoot, const QString &proRel, const QString &exe);

// The real path of an existing file or folder, empty when it does not exist. Unix: QFileInfo::canonicalFilePath.
// Windows: also resolves junctions (Qt resolves only symlinks), e.g. Oracle's Program Files\Java\latest\jdk-25 -> the
// real JDK folder, so the same folder reached two ways compares equal.
QString realPath(const QString &path);

// JDKs installed where only this OS puts them (JdkScanner, LspServers::findJava), on top of the folders those list
// themselves (JAVA_HOME, PATH, ~/.jdks, ~/.gradle/jdks, SDKMAN, /usr/lib/jvm …). `source` is what the JDK dialog shows.
// - jdkFolders: folders whose subfolders are JDKs. Unix: none. Windows: Program Files\Java, Eclipse Adoptium, Zulu,
//   Amazon Corretto, BellSoft, Semeru (those that exist).
// - jdkHomes: JDK folders themselves. Unix: none. Windows: those the registry names (JavaSoft, Adoptium, Eclipse
//   Foundation, Microsoft, Azul), Program Files\Microsoft\jdk-*, and Scoop's JDK apps (…\scoop\apps\<jdk>\current).
struct JdkPlace {
    QString path;
    QString source;
};
QList<JdkPlace> jdkFolders();
QList<JdkPlace> jdkHomes();

// The folders the C / C++ compiler searches for <headers> (#include completion), those that exist. Unix: the newest
// /usr/include/c++/<n>, the multiarch folders, /usr/local/include, /usr/include. Windows: MSVC's include folder and the
// Windows SDK's ucrt / um / shared / winrt when Visual Studio's C++ tools are installed, else MSYS2's MinGW headers
// (ucrt64, clang64 or mingw64, with its libstdc++ folders).
QStringList systemIncludeDirs();

// The git executable, empty when there is none. PATH first; on Windows also Git for Windows' usual install folders,
// for an install that did not add itself to PATH.
QString gitExecutable();

// Folders where per-user tools land that a desktop-launched QODE may lack on PATH, beyond the home-relative ones the
// callers add themselves (~/.local/bin, ~/.bun/bin, ~/.cargo/bin …, the same on every OS). Unix: none. Windows: npm's
// global folder, pnpm's, Node.js's, WinGet's links and Scoop's shims. Only folders that exist.
QStringList extraBinDirs();

// More folders where a language server (clangd) may be installed without being on PATH, looked at only by
// LspServers::locate (not added to any PATH: they hold many other tools). Unix: none. Windows: LLVM's Program Files
// folder and MSYS2's ucrt64 / clang64 / mingw64 bin folders. Only folders that exist.
QStringList languageServerDirs();

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

// The shells the QODE chat's run_command can run its commands in (AgentTools), the default first:
// - id: what the setting stores: "bash", "powershell", "gitbash".
// - name: what the model and the user are told: "bash", "Windows PowerShell 5.1", "PowerShell 7", "Git Bash".
// - dialect: the language the model writes the command in: "bash" or "powershell".
// Unix: bash (/bin/bash) only. Windows: PowerShell (pwsh when installed, else Windows PowerShell 5.1, which every
// Windows has), then Git Bash when Git for Windows has one.
struct ChatShell {
    QString id;
    QString name;
    QString dialect;
    QString program;
};
QList<ChatShell> chatShells();
// The shell with that id, else the default (the first of chatShells).
ChatShell chatShell(const QString &id);
// Sets `process` up to run `command` (written for shell.dialect) in `shell`; call it after setProcessEnvironment (it
// adds to that environment) and start() the process afterwards. Unix: bash -c command, as before. Windows, PowerShell:
// the command goes in the environment variable QODE_CHAT_SCRIPT (a temporary file, deleted with `process`, when it is
// too long for one), and a fixed -Command line runs it: no quoting, errors as text (-EncodedCommand would give CLIXML),
// UTF-8 output, no progress bars, -ExecutionPolicy Bypass, and bash's exit code: 0 when the last command succeeded,
// else the native program's exit code (1 for a failed cmdlet). Git Bash: bash.exe -c command.
void setChatShellCommand(QProcess *process, const ChatShell &shell, const QString &command);
// A path a shell or a model wrote, in Qt's form. Unix: unchanged. Windows: '\' becomes '/', and the drive forms of
// Git Bash, Cygwin and WSL (/c/Users/x, /cygdrive/c/x, /mnt/c/x) become C:/Users/x when that drive exists.
QString fromShellPath(const QString &path);

// Starts QODE again once this process has ended; call it after the window closed (the project claim goes with the
// process). Unix: a shell that waits a second, then execs QODE. Windows: a helper that waits for this process to exit.
void restartSelf();

} // namespace Platform
