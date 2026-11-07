#include "platform/Platform.h"
#include "platform/AppPaths.h"
#include "terminal/ShellSyntax.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringConverter>
#include <QThread>
#include <QUrl>
#include <QVersionNumber>
#include <QtGlobal>

#include <algorithm>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>

namespace {

qint64 fileTimeTo100ns(const FILETIME &t)
{
    return (qint64(t.dwHighDateTime) << 32) | qint64(t.dwLowDateTime);
}

const char kRestartArg[] = "--qode-restart-after";

std::wstring ownExe()
{
    wchar_t buf[MAX_PATH * 4];
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return len > 0 && len < std::size(buf) ? std::wstring(buf, len) : std::wstring();
}

// Starts this binary with `args` (already quoted as needed), with no console and not waited for.
bool startOwnExe(const std::wstring &args)
{
    const std::wstring exe = ownExe();
    if (exe.empty())
        return false;
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? std::wstring() : L" " + args);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool Platform::runIfHelper(int argc, char *argv[])
{
    // restartSelf(): wait (at most 30 s) until the old QODE has exited and released its project, then start QODE.
    if (argc != 3 || std::strcmp(argv[1], kRestartArg) != 0)
        return false;
    const DWORD pid = DWORD(std::strtoul(argv[2], nullptr, 10));
    if (const HANDLE old = pid > 0 ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr) {
        ::WaitForSingleObject(old, 30000);
        ::CloseHandle(old);
    }
    startOwnExe({});
    return true;
}

void Platform::initProcess()
{
    // Inherited by every child, the terminal's shells too: Python then reads and writes UTF-8.
    if (!qEnvironmentVariableIsSet("PYTHONUTF8"))
        qputenv("PYTHONUTF8", "1");
    if (!qEnvironmentVariableIsSet("PYTHONIOENCODING"))
        qputenv("PYTHONIOENCODING", "utf-8");
}

QString Platform::decodeOutput(const QByteArray &bytes)
{
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString text = utf8.decode(bytes);
    if (!utf8.hasError())
        return text;
    // The system's ANSI code page, which children use. GetACP() would give QODE's own (UTF-8, from its manifest), so it
    // comes from where Windows keeps it.
    static const UINT acp = [] {
        const QSettings nls(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage"), QSettings::NativeFormat);
        const UINT cp = nls.value(QStringLiteral("ACP")).toString().toUInt();
        return cp ? cp : 1252u;
    }();
    const int n = ::MultiByteToWideChar(acp, 0, bytes.constData(), int(bytes.size()), nullptr, 0);
    if (n <= 0)
        return QString::fromLatin1(bytes);
    QString out(n, Qt::Uninitialized);
    ::MultiByteToWideChar(acp, 0, bytes.constData(), int(bytes.size()), reinterpret_cast<wchar_t *>(out.data()), n);
    return out;
}

qint64 Platform::msBeforeMain()
{
    FILETIME created, exited, kernel, user, now;
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return -1;
    ::GetSystemTimePreciseAsFileTime(&now);
    const qint64 diff = fileTimeTo100ns(now) - fileTimeTo100ns(created);
    return diff >= 0 ? diff / 10000 : -1;
}

QString Platform::userKey()
{
    // The SID ("S-1-5-21-…") is unique per user and never changes, unlike the user name.
    QString key;
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size > 0) {
            QByteArray buf(int(size), '\0');
            if (::GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
                LPWSTR sid = nullptr;
                if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &sid)) {
                    key = QString::fromWCharArray(sid);
                    ::LocalFree(sid);
                }
            }
        }
        ::CloseHandle(token);
    }
    if (key.isEmpty())
        key = qEnvironmentVariable("USERNAME", QStringLiteral("user"));
    return key;
}

bool Platform::replaceSymlink(const QString &, const QString &)
{
    return false;
}

namespace {

// /d: no AutoRun commands from the registry. /s /c "<line>": cmd drops the outer quotes and runs the rest as it is, so
// the line keeps its own quotes. QProcess would quote each argument for the C runtime, which cmd does not parse, so the
// line goes in as native arguments.
void setCmdLine(QProcess *process, const QString &cmdLine)
{
    process->setProgram(qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe")));
    process->setArguments({});
    process->setNativeArguments(QStringLiteral("/d /s /c \"") + cmdLine + QLatin1Char('"'));
}

} // namespace

void Platform::setShellCommand(QProcess *process, const QString &command)
{
    // Callers write sh (Spring's JAVA_TOOL_OPTIONS='…' .\gradlew bootRun): rewritten for cmd as in the terminal. A line
    // outside what ShellSyntax knows (one the user typed for cmd) goes on unchanged.
    setCmdLine(process, ShellSyntax::translate(command, QStringLiteral("cmd.exe")));
}

namespace {

// One argument for a cmd.exe command line: quoted when it holds a space or a character cmd treats specially, with
// embedded quotes doubled (what batch files expect).
QString cmdQuote(const QString &arg)
{
    if (arg.isEmpty())
        return QStringLiteral("\"\"");
    static const QString special = QStringLiteral(" \t\"&|<>^(),;=");
    bool plain = true;
    for (const QChar c : arg)
        if (special.contains(c)) {
            plain = false;
            break;
        }
    if (plain)
        return arg;
    QString q = arg;
    q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

} // namespace

void Platform::setCommand(QProcess *process, const QString &program, const QStringList &args)
{
    QString exe = program;
    if (!exe.contains(QLatin1Char('/')) && !exe.contains(QLatin1Char('\\'))) {
        const QString found = QStandardPaths::findExecutable(exe); // adds PATHEXT: gradle -> gradle.bat
        if (!found.isEmpty())
            exe = found;
    }
    const QString suffix = QFileInfo(exe).suffix().toLower();
    if (suffix != QLatin1String("bat") && suffix != QLatin1String("cmd")) {
        process->setProgram(exe);
        process->setArguments(args);
        return;
    }
    QString line = cmdQuote(QDir::toNativeSeparators(exe));
    for (const QString &a : args)
        line += QLatin1Char(' ') + cmdQuote(a);
    setCmdLine(process, line);
}

QString Platform::wrapperCommand(const QString &dir, const QString &name)
{
    return wrapperFile(dir, name).isEmpty() ? QString() : QStringLiteral(".\\") + name;
}

QString Platform::wrapperFile(const QString &dir, const QString &name)
{
    for (const char *ext : {".bat", ".cmd"}) {
        const QString path = dir + QLatin1Char('/') + name + QLatin1String(ext);
        if (QFileInfo(path).isFile())
            return path;
    }
    return {};
}

bool Platform::wrapperNeedsExecBit(const QString &, const QString &)
{
    return false;
}

QString Platform::venvBinDir()
{
    return QStringLiteral("Scripts");
}

QString Platform::venvProgram(const QString &venv, const QString &name)
{
    const QFileInfo fi(venv + QStringLiteral("/Scripts/") + name + QStringLiteral(".exe"));
    return fi.isFile() ? fi.absoluteFilePath() : QString();
}

bool Platform::hasExecBit()
{
    return false;
}

QString Platform::invalidFileName(const QString &name)
{
    for (const QChar c : name)
        if (c.unicode() < 32 || QStringLiteral("<>:\"|?*\\").contains(c))
            return QStringLiteral("A name on Windows cannot contain %1.")
                .arg(c.unicode() < 32 ? QStringLiteral("control characters") : QStringLiteral("'%1'").arg(c));
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        return QStringLiteral("A name on Windows cannot end with a dot or a space.");
    // Device names, with or without an extension (CON, nul.txt, Com1.tar.gz), also with spaces before the dot.
    static const QRegularExpression device(QStringLiteral("^(con|prn|aux|nul|com[1-9\\x{b9}\\x{b2}\\x{b3}]|lpt[1-9\\x{b9}\\x{b2}\\x{b3}])$"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QString stem = name.section(QLatin1Char('.'), 0, 0).trimmed();
    if (device.match(stem).hasMatch())
        return QStringLiteral("\"%1\" is reserved by Windows (a device name).").arg(stem);
    return {};
}

bool Platform::canMakeSymlinks()
{
    return false;
}

bool Platform::watchesBlockRenames()
{
    return true;
}

int Platform::busyFileRetries()
{
    return 3;
}

bool Platform::hiddenByOs(const QFileInfo &info)
{
    const QString name = info.fileName();
    return info.isHidden() || name.compare(QLatin1String("desktop.ini"), Qt::CaseInsensitive) == 0
        || name.compare(QLatin1String("Thumbs.db"), Qt::CaseInsensitive) == 0;
}

QString Platform::legacyEncoding()
{
    return QStringLiteral("Windows-1252");
}

QString Platform::pathKey(const QString &path)
{
    return QDir::fromNativeSeparators(path).toLower();
}

bool Platform::samePath(const QString &a, const QString &b)
{
    return pathKey(a) == pathKey(b);
}

bool Platform::isUnder(const QString &path, const QString &root)
{
    if (root.isEmpty())
        return false;
    const QString r = pathKey(root);
    return pathKey(path).startsWith(r.endsWith(QLatin1Char('/')) ? r : r + QLatin1Char('/'));
}

QString Platform::uriToPath(const QString &uri)
{
    // QUrl decodes %3A; the drive letter comes in whatever case the server chose.
    QString path = QUrl(uri).toLocalFile();
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':') && path.at(0).isLetter())
        path[0] = path.at(0).toUpper();
    return path;
}

QString Platform::pathToUri(const QString &path)
{
    return QUrl::fromLocalFile(path).toString();
}

QString Platform::exeSuffix()
{
    return QStringLiteral(".exe");
}

QString Platform::pythonCommand()
{
    // python.exe may be the Microsoft Store stub (WindowsApps), which opens the Store; py is only there when Python is.
    return QStandardPaths::findExecutable(QStringLiteral("py")).isEmpty() ? QStringLiteral("python") : QStringLiteral("py");
}

namespace {

// Git Bash: bin\bash.exe in Git's folder; git.exe sits in cmd\, bin\ or mingw64\bin\ of it. Empty when there is none.
QString gitBash()
{
    const QString git = Platform::gitExecutable();
    if (git.isEmpty())
        return {};
    QDir dir = QFileInfo(git).dir();
    for (int up = 0; up < 2 && dir.cdUp(); ++up)
        if (QFileInfo(dir.filePath(QStringLiteral("bin/bash.exe"))).isFile())
            return dir.filePath(QStringLiteral("bin/bash.exe"));
    return {};
}

} // namespace

QString Platform::bashProgram()
{
    const QString bash = gitBash();
    return bash.isEmpty() ? QStringLiteral("bash") : bash;
}

// --- qmake projects ---------------------------------------------------------------------------------------------------

namespace {

// vcvars64.bat of the newest Visual Studio with the C++ tools, "" when there is none. vswhere is asked once.
QString vcvars64()
{
    static const QString found = [] {
        const QString x86 = qEnvironmentVariable("ProgramFiles(x86)", QStringLiteral("C:\\Program Files (x86)"));
        const QString vswhere = x86 + QStringLiteral("\\Microsoft Visual Studio\\Installer\\vswhere.exe");
        if (!QFileInfo(vswhere).isFile())
            return QString();
        QProcess p;
        p.start(vswhere, {QStringLiteral("-latest"), QStringLiteral("-products"), QStringLiteral("*"), QStringLiteral("-requires"),
                          QStringLiteral("Microsoft.VisualStudio.Component.VC.Tools.x86.x64"), QStringLiteral("-property"),
                          QStringLiteral("installationPath")});
        if (!p.waitForFinished(5000)) {
            p.kill();
            p.waitForFinished(1000);
            return QString();
        }
        const QString dir = QString::fromLocal8Bit(p.readAllStandardOutput()).section(QLatin1Char('\n'), 0, 0).trimmed();
        const QString bat = dir + QStringLiteral("\\VC\\Auxiliary\\Build\\vcvars64.bat");
        return !dir.isEmpty() && QFileInfo(bat).isFile() ? bat : QString();
    }();
    return found;
}

bool isMsvcKit(const QString &qmake)
{
    return QFileInfo(qmake).absolutePath().contains(QLatin1String("msvc"), Qt::CaseInsensitive);
}

// The Qt installer's root (…\Qt) of a qmake at <root>\<version>\<kit>\bin\qmake.exe.
QString qtRootOf(const QString &qmake)
{
    QDir dir = QFileInfo(qmake).absoluteDir();
    for (int i = 0; i < 3; ++i)
        if (!dir.cdUp())
            return {};
    return dir.absolutePath();
}

// For a batch file: native separators, and % doubled (a batch file expands %…%).
QString batPath(const QString &path)
{
    QString p = QDir::toNativeSeparators(path);
    return p.replace(QLatin1Char('%'), QStringLiteral("%%"));
}

} // namespace

QStringList Platform::qmakeCandidates()
{
    struct Kit {
        QVersionNumber version;
        QString qmake;
        bool msvc;
    };
    QList<Kit> kits;
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo &drive : drives) {
        const QDir qt(drive.absoluteFilePath() + QStringLiteral("Qt"));
        if (!qt.exists())
            continue;
        const QStringList versions = qt.entryList({QStringLiteral("[0-9]*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &v : versions) {
            const QVersionNumber version = QVersionNumber::fromString(v);
            if (version.isNull())
                continue;
            const QStringList kitDirs = QDir(qt.filePath(v)).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &kit : kitDirs) {
                const QString qmake = qt.filePath(v + QLatin1Char('/') + kit + QStringLiteral("/bin/qmake.exe"));
                const bool msvc = kit.startsWith(QLatin1String("msvc")) && !kit.contains(QLatin1String("arm64"));
                const bool mingw = kit.startsWith(QLatin1String("mingw"));
                if ((msvc || mingw) && QFileInfo(qmake).isFile())
                    kits.append({version, qmake, msvc});
            }
        }
    }
    // Newest first; within a version the kit whose compiler is there.
    const bool haveMsvc = !vcvars64().isEmpty();
    std::stable_sort(kits.begin(), kits.end(), [haveMsvc](const Kit &a, const Kit &b) {
        if (a.version != b.version)
            return a.version > b.version;
        return (a.msvc == haveMsvc) && (b.msvc != haveMsvc);
    });
    QStringList out;
    for (const Kit &k : std::as_const(kits))
        out << k.qmake;
    return out;
}

QStringList Platform::qmakeScopes(const QString &qmake)
{
    QStringList scopes{QStringLiteral("win32"), QStringLiteral("windows")};
    if (qmake.isEmpty())
        return scopes;
    if (isMsvcKit(qmake))
        scopes << QStringLiteral("msvc") << QStringLiteral("win32-msvc");
    else
        scopes << QStringLiteral("mingw") << QStringLiteral("win32-g++") << QStringLiteral("gcc");
    return scopes;
}

QString Platform::qmakeRunCommand(const QString &qmake, const QString &projectRoot, const QString &proRel, const QString &exe)
{
    if (qmake.isEmpty() || projectRoot.isEmpty())
        return {};
    const QString root = qtRootOf(qmake);
    QString setup, make;
    if (isMsvcKit(qmake)) {
        const QString vc = vcvars64();
        if (vc.isEmpty())
            return {};
        setup = QStringLiteral("call \"%1\" >nul || (echo QODE: setting up the Visual Studio compiler failed& exit /b 1)").arg(batPath(vc));
        const QString jom = root + QStringLiteral("/Tools/jom/jom.exe");
        if (QFileInfo(jom).isFile())
            make = QLatin1Char('"') + batPath(jom) + QLatin1Char('"');
        else if (!QStandardPaths::findExecutable(QStringLiteral("jom")).isEmpty())
            make = QStringLiteral("jom");
        else
            make = QStringLiteral("nmake /nologo");
    } else {
        // The kit's MinGW comes with the Qt installer (Tools\mingw<ver>_64); else one already on PATH.
        const QStringList mingws = QDir(root + QStringLiteral("/Tools")).entryList({QStringLiteral("mingw*")}, QDir::Dirs, QDir::Name);
        for (auto it = mingws.crbegin(); it != mingws.crend(); ++it) {
            const QString bin = root + QStringLiteral("/Tools/") + *it + QStringLiteral("/bin");
            if (QFileInfo(bin + QStringLiteral("/mingw32-make.exe")).isFile()) {
                setup = QStringLiteral("set \"PATH=%1;%PATH%\"").arg(batPath(bin));
                break;
            }
        }
        make = QStringLiteral("mingw32-make -j%1").arg(qMax(1, QThread::idealThreadCount()));
    }
    // .\ for one in build\: a bare name may not be looked up in the current folder (NoDefaultCurrentDirectoryInExePath).
    const QString run = (QDir::isAbsolutePath(exe) ? exe : QStringLiteral("./") + exe) + QStringLiteral(".exe");
    const QStringList lines = {
        QStringLiteral("@echo off"),
        QStringLiteral("rem Written by QODE: builds %1 into build\\ and runs it.").arg(batPath(proRel)),
        QStringLiteral("setlocal"),
        setup,
        // The program finds Qt's DLLs there.
        QStringLiteral("set \"PATH=%1;%PATH%\"").arg(batPath(QFileInfo(qmake).absolutePath())),
        QStringLiteral("cd /d \"%1\" || exit /b 1").arg(batPath(projectRoot)),
        QStringLiteral("if not exist build\\ md build"),
        QStringLiteral("cd build || exit /b 1"),
        // One release build, straight into build\ (no release\ and debug\ folders).
        QStringLiteral("\"%1\" \"..\\%2\" \"CONFIG+=release\" \"CONFIG-=debug_and_release\" || exit /b 1").arg(batPath(qmake), batPath(proRel)),
        make + QStringLiteral(" || exit /b 1"),
        QLatin1Char('"') + batPath(run) + QLatin1Char('"'),
    };
    const QString dir = AppPaths::cache(QStringLiteral("qmake-run"));
    const QByteArray hash = QCryptographicHash::hash(QDir::cleanPath(projectRoot).toLower().toUtf8(), QCryptographicHash::Sha1).toHex().left(10);
    const QString file = dir + QLatin1Char('/') + QFileInfo(projectRoot).fileName() + QLatin1Char('-') + QString::fromLatin1(hash) + QStringLiteral(".bat");
    const QByteArray content = lines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n";
    QFile current(file);
    if (!current.open(QIODevice::ReadOnly) || current.readAll() != content) {
        current.close();
        QDir().mkpath(dir);
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(content) != content.size() || !out.commit())
            return {};
    }
    QString quoted = file;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

namespace {

QStringList programFilesRoots() // 64-bit first; the same folder twice when QODE is 64-bit is dropped
{
    QStringList out;
    for (const char *var : {"ProgramW6432", "ProgramFiles"}) {
        const QString dir = QDir::fromNativeSeparators(qEnvironmentVariable(var));
        if (!dir.isEmpty() && !out.contains(dir, Qt::CaseInsensitive))
            out << dir;
    }
    if (out.isEmpty())
        out << QStringLiteral("C:/Program Files");
    return out;
}

} // namespace

QString Platform::realPath(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    // FILE_FLAG_BACKUP_SEMANTICS opens folders too; the handle follows every junction and symlink on the way.
    const HANDLE h = ::CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(canonical).utf16()), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return canonical;
    wchar_t buf[4096];
    const DWORD n = ::GetFinalPathNameByHandleW(h, buf, DWORD(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    ::CloseHandle(h);
    if (n == 0 || n >= std::size(buf))
        return canonical;
    QString real = QString::fromWCharArray(buf, int(n));
    if (real.startsWith(QLatin1String("\\\\?\\UNC\\")))
        real = QStringLiteral("\\\\") + real.mid(8);
    else if (real.startsWith(QLatin1String("\\\\?\\")))
        real = real.mid(4);
    return QDir::fromNativeSeparators(real);
}

QList<Platform::JdkPlace> Platform::jdkFolders()
{
    QList<JdkPlace> out;
    for (const QString &root : programFilesRoots())
        for (const char *vendor : {"Java", "Eclipse Adoptium", "Zulu", "Amazon Corretto", "BellSoft", "Semeru", "IBM/Semeru"})
            if (const QString dir = root + QLatin1Char('/') + QLatin1String(vendor); QFileInfo(dir).isDir())
                out.append({dir, QStringLiteral("System")});
    return out;
}

QList<Platform::JdkPlace> Platform::jdkHomes()
{
    QList<JdkPlace> out;
    const auto add = [&out](const QString &home, const QString &source) {
        const QString dir = QDir::fromNativeSeparators(home.trimmed());
        if (!dir.isEmpty() && QFileInfo(dir).isDir())
            out.append({dir, source});
    };
    // The installers' own keys: <key>\<version>[\<sub>] holds the folder in <value>.
    struct Key {
        const char *key, *sub, *value;
    };
    for (const Key &k : {Key{"JavaSoft\\JDK", "", "JavaHome"}, Key{"JavaSoft\\Java Development Kit", "", "JavaHome"},
                         Key{"Eclipse Adoptium\\JDK", "hotspot/MSI/", "Path"}, Key{"Eclipse Foundation\\JDK", "hotspot/MSI/", "Path"},
                         Key{"Microsoft\\JDK", "hotspot/MSI/", "Path"}, Key{"Azul Systems\\Zulu", "", "InstallationPath"}}) {
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\") + QLatin1String(k.key), QSettings::NativeFormat);
        for (const QString &version : reg.childGroups())
            add(reg.value(version + QLatin1Char('/') + QLatin1String(k.sub) + QLatin1String(k.value)).toString(), QStringLiteral("System"));
    }
    // Microsoft's build of OpenJDK installs into Program Files\Microsoft\jdk-<version> (next to unrelated folders).
    for (const QString &root : programFilesRoots()) {
        const QDir ms(root + QStringLiteral("/Microsoft"));
        for (const QString &name : ms.entryList({QStringLiteral("jdk-*")}, QDir::Dirs | QDir::NoDotAndDotDot))
            add(ms.filePath(name), QStringLiteral("System"));
    }
    // Scoop: per user (%SCOOP% or ~\scoop) and global (%ProgramData%\scoop); a JDK app's current version is "current".
    QStringList scoop{qEnvironmentVariable("SCOOP"), QDir::homePath() + QStringLiteral("/scoop"), qEnvironmentVariable("SCOOP_GLOBAL")};
    if (const QString data = qEnvironmentVariable("ProgramData"); !data.isEmpty())
        scoop << data + QStringLiteral("/scoop");
    static const QRegularExpression jdkApp(QStringLiteral("jdk|jre|java|temurin|zulu|corretto|liberica|graalvm|semeru"),
                                           QRegularExpression::CaseInsensitiveOption);
    QStringList seen;
    for (const QString &root : std::as_const(scoop)) {
        const QString apps = QDir::fromNativeSeparators(root) + QStringLiteral("/apps");
        if (root.isEmpty() || seen.contains(apps, Qt::CaseInsensitive))
            continue;
        seen << apps;
        for (const QString &app : QDir(apps).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (jdkApp.match(app).hasMatch())
                add(apps + QLatin1Char('/') + app + QStringLiteral("/current"), QStringLiteral("Scoop"));
    }
    return out;
}

QString Platform::gitExecutable()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (!onPath.isEmpty())
        return onPath;
    // Git for Windows: all users (Program Files), or per user (LocalAppData\Programs).
    QStringList roots{qEnvironmentVariable("ProgramW6432"), qEnvironmentVariable("ProgramFiles")};
    if (const QString local = qEnvironmentVariable("LOCALAPPDATA"); !local.isEmpty())
        roots << local + QStringLiteral("\\Programs");
    for (const QString &root : std::as_const(roots)) {
        if (root.isEmpty())
            continue;
        const QString git = QDir::fromNativeSeparators(root) + QStringLiteral("/Git/cmd/git.exe");
        if (QFileInfo(git).isFile())
            return git;
    }
    return {};
}

QStringList Platform::extraBinDirs()
{
    QStringList out;
    const auto add = [&out](const QString &dir) {
        if (!dir.isEmpty() && QFileInfo(dir).isDir())
            out << QDir::toNativeSeparators(dir);
    };
    const QString appData = qEnvironmentVariable("APPDATA"), local = qEnvironmentVariable("LOCALAPPDATA");
    const QString programFiles = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:\\Program Files"));
    if (!appData.isEmpty())
        add(appData + QStringLiteral("\\npm")); // npm install -g
    if (!local.isEmpty()) {
        add(local + QStringLiteral("\\pnpm"));
        add(local + QStringLiteral("\\Microsoft\\WinGet\\Links"));
        add(local + QStringLiteral("\\Volta\\bin"));
    }
    add(programFiles + QStringLiteral("\\nodejs"));
    add(QDir::homePath() + QStringLiteral("/scoop/shims"));
    return out;
}

QList<Platform::Shell> Platform::terminalShells()
{
    QList<Shell> out;
    const auto add = [&out](const QString &name, const QString &program) {
        if (!program.isEmpty() && QFileInfo(program).isFile())
            out.append({name, QDir::toNativeSeparators(program)});
    };
    const QString system = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")) + QStringLiteral("\\System32");
    add(QStringLiteral("PowerShell"), QStandardPaths::findExecutable(QStringLiteral("pwsh")));
    add(QStringLiteral("Windows PowerShell"), system + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe"));
    add(QStringLiteral("Command Prompt"), qEnvironmentVariable("ComSpec", system + QStringLiteral("\\cmd.exe")));
    add(QStringLiteral("Git Bash"), gitBash());
    // Without a distribution wsl.exe only explains how to install one.
    const QSettings lxss(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Lxss"), QSettings::NativeFormat);
    if (!lxss.childGroups().isEmpty())
        add(QStringLiteral("WSL"), system + QStringLiteral("\\wsl.exe"));
    return out;
}

QString Platform::processCwd(qint64)
{
    return {};
}

namespace {

// When process `pid` started (100 ns units), 0 when it cannot be asked.
qint64 creationTime(DWORD pid)
{
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    FILETIME created, exited, kernel, user;
    const bool ok = ::GetProcessTimes(h, &created, &exited, &kernel, &user);
    ::CloseHandle(h);
    return ok ? fileTimeTo100ns(created) : 0;
}

} // namespace

bool Platform::shellInForeground(qint64 pid)
{
    if (pid <= 0)
        return false;
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    // A console has no foreground process group: anything the shell started counts, as long as it is newer than the
    // shell (a parent id is only a number, and an older process can carry one that was reused).
    const qint64 shellStart = creationTime(DWORD(pid));
    bool child = false;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    for (BOOL ok = ::Process32FirstW(snap, &e); ok && !child; ok = ::Process32NextW(snap, &e)) {
        if (e.th32ParentProcessID != DWORD(pid) || e.th32ProcessID == DWORD(pid))
            continue;
        const qint64 started = creationTime(e.th32ProcessID);
        child = shellStart == 0 || started == 0 || started >= shellStart;
    }
    ::CloseHandle(snap);
    return !child;
}

void Platform::killTree(QProcess *process)
{
    if (!process || process->state() == QProcess::NotRunning)
        return;
    const DWORD root = DWORD(process->processId());
    // Collect the descendants first (a parent id is only a number: a child must be newer than its parent), then end
    // them, deepest last found first, and the process itself through QProcess so it sees the end.
    QList<DWORD> tree;
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (root && snap != INVALID_HANDLE_VALUE) {
        QList<QPair<DWORD, DWORD>> all; // (pid, parent)
        PROCESSENTRY32W e;
        e.dwSize = sizeof e;
        for (BOOL ok = ::Process32FirstW(snap, &e); ok; ok = ::Process32NextW(snap, &e))
            all.append({e.th32ProcessID, e.th32ParentProcessID});
        QList<DWORD> parents{root};
        for (int i = 0; i < parents.size(); ++i) {
            const qint64 parentStart = creationTime(parents.at(i));
            for (const auto &[pid, parent] : std::as_const(all)) {
                if (parent != parents.at(i) || pid == root || parents.contains(pid))
                    continue;
                const qint64 started = creationTime(pid);
                if (parentStart && started && started < parentStart)
                    continue;
                parents << pid;
                tree << pid;
            }
        }
    }
    if (snap != INVALID_HANDLE_VALUE)
        ::CloseHandle(snap);
    for (auto it = tree.crbegin(); it != tree.crend(); ++it)
        if (const HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, *it)) {
            ::TerminateProcess(h, 1);
            ::CloseHandle(h);
        }
    process->kill();
}

void Platform::restartSelf()
{
    startOwnExe(QString::fromLatin1(kRestartArg).toStdWString() + L" " + std::to_wstring(::GetCurrentProcessId()));
}
