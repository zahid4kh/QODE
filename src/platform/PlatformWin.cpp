#include "platform/Platform.h"
#include "platform/AppPaths.h"
#include "terminal/ShellSyntax.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStyleHints>
#include <QTemporaryFile>
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
#include <shlobj.h>
#include <tlhelp32.h>
#include <winioctl.h>

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

bool g_setQpaPlatform = false; // initProcess set QT_QPA_PLATFORM, initGui removes it again

// With QT_QPA_PLATFORM=windows:altgr, text typed with AltGr comes with GroupSwitchModifier instead of Ctrl+Alt. The
// shortcut map has already seen the key (as a ShortcutOverride) when the key press is sent, so the press itself goes on
// without that modifier: widgets see plain typing, as on Linux.
class AltGrFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->modifiers() & Qt::GroupSwitchModifier)
                key->setModifiers(key->modifiers() & ~Qt::GroupSwitchModifier);
        }
        return QObject::eventFilter(watched, event);
    }
};

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
    // Read by the QApplication constructor (an environment variable, not "-platform" in argv: a changed argv makes Qt
    // take the arguments from argv instead of the Unicode command line).
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "windows:altgr");
        g_setQpaPlatform = true;
    }
}

void Platform::initGui()
{
    if (g_setQpaPlatform)
        qunsetenv("QT_QPA_PLATFORM");
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(new AltGrFilter(app));
}

void Platform::setDarkTheme(bool dark)
{
    // Each request makes Qt refresh its theme (palettes, fonts), so only a change is sent. The first call (from the
    // Application constructor, before any window exists) always is, which also stops the Windows setting from
    // switching the title bars later.
    static int requested = -1;
    if (requested == int(dark) || !qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return;
    requested = int(dark);
    QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
}

bool Platform::selectInFileManager(const QString &file)
{
    // COM is initialised on the GUI thread (Qt's OleInitialize).
    const std::wstring native = QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()).toStdWString();
    PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(native.c_str());
    if (!item)
        return false;
    const HRESULT hr = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ::ILFree(item);
    return SUCCEEDED(hr);
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

std::wstring wide(const QString &path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

// A directory junction `link` -> `target` (an absolute folder). The reparse data is the mount-point layout from
// ntifs.h, which user-mode headers do not declare: tag, sizes, then "\??\target" and "target", each NUL-ended.
bool makeJunction(const QString &target, const QString &link)
{
    const std::wstring substitute = L"\\??\\" + wide(QFileInfo(target).absoluteFilePath());
    const std::wstring print = wide(QFileInfo(target).absoluteFilePath());
    const WORD pathBytes = WORD((substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t));
    struct Header {
        DWORD tag;
        WORD dataLength, reserved;
        WORD substituteOffset, substituteLength, printOffset, printLength;
    };
    std::string buf(sizeof(Header) + pathBytes, '\0');
    auto *h = reinterpret_cast<Header *>(buf.data());
    h->tag = IO_REPARSE_TAG_MOUNT_POINT;
    h->dataLength = WORD(8 + pathBytes); // the four offset / length words plus the paths
    h->substituteOffset = 0;
    h->substituteLength = WORD(substitute.size() * sizeof(wchar_t));
    h->printOffset = WORD((substitute.size() + 1) * sizeof(wchar_t));
    h->printLength = WORD(print.size() * sizeof(wchar_t));
    auto *paths = reinterpret_cast<wchar_t *>(buf.data() + sizeof(Header));
    std::memcpy(paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(paths + substitute.size() + 1, print.c_str(), (print.size() + 1) * sizeof(wchar_t));

    const std::wstring linkW = wide(link);
    if (!::CreateDirectoryW(linkW.c_str(), nullptr))
        return false;
    const HANDLE dir = ::CreateFileW(linkW.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    bool ok = false;
    if (dir != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        ok = ::DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, buf.data(), DWORD(buf.size()), nullptr, 0, &returned, nullptr);
        ::CloseHandle(dir);
    }
    if (!ok)
        ::RemoveDirectoryW(linkW.c_str());
    return ok;
}

bool isJunction(const QString &path)
{
    const DWORD attrs = ::GetFileAttributesW(wide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

namespace {

bool removeEntry(const std::wstring &path)
{
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return ::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND;
    if (attrs & FILE_ATTRIBUTE_READONLY)
        ::SetFileAttributesW(path.c_str(), attrs & ~DWORD(FILE_ATTRIBUTE_READONLY));
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return ::DeleteFileW(path.c_str()) != 0;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) // a junction or folder symlink: the link goes, its target stays
        return ::RemoveDirectoryW(path.c_str()) != 0;
    bool ok = true;
    WIN32_FIND_DATAW d;
    const HANDLE find = ::FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = d.cFileName;
            if (name != L"." && name != L"..")
                ok = removeEntry(path + L"\\" + name) && ok;
        } while (::FindNextFileW(find, &d));
        ::FindClose(find);
    }
    return ::RemoveDirectoryW(path.c_str()) && ok;
}

} // namespace

bool Platform::removeTree(const QString &path)
{
    if (path.isEmpty())
        return false;
    // \\?\ lifts the 260-character limit for paths QODE did not shorten (deep node_modules).
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QLatin1String("\\\\")))
        native = QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    else
        native = QStringLiteral("\\\\?\\") + native;
    return removeEntry(native.toStdWString());
}

bool Platform::setCurrentVersion(const QString &root, const QString &version)
{
    const QString link = root + QStringLiteral("/current");
    // Only ever replace a link (RemoveDirectoryW on a junction removes the link, never what it points at) or an empty
    // folder, never a real folder with files.
    if (::GetFileAttributesW(wide(link).c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!isJunction(link) && !QDir(link).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            return false;
        if (!::RemoveDirectoryW(wide(link).c_str()))
            return false;
    }
    return makeJunction(root + QLatin1Char('/') + version, link);
}

QString Platform::currentVersion(const QString &root)
{
    const QString link = root + QStringLiteral("/current");
    if (!isJunction(link))
        return {};
    const QString real = realPath(link);
    return real.isEmpty() || samePath(real, link) ? QString() : QFileInfo(real).fileName();
}

QString Platform::commandLinkFile(const QString &link)
{
    return link + QStringLiteral(".cmd");
}

bool Platform::writeCommandLink(const QString &target, const QString &file)
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write("@\"" + QDir::toNativeSeparators(target).toUtf8() + "\" %*\r\n");
    return f.commit();
}

QString Platform::commandLinkTarget(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4096)
        return {};
    static const QRegularExpression link(QStringLiteral("^@\"([^\"]+)\" %\\*\\s*$"));
    const auto m = link.match(QString::fromUtf8(f.readAll()));
    return m.hasMatch() ? QDir::fromNativeSeparators(m.captured(1)) : QString();
}

void Platform::setUnpackCommand(QProcess *process, const QString &archive, const QString &dir)
{
    const QString system = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    process->setProgram(QDir::fromNativeSeparators(system) + QStringLiteral("/System32/tar.exe"));
    process->setArguments({QStringLiteral("-xf"), QDir::toNativeSeparators(archive), QStringLiteral("-C"), QDir::toNativeSeparators(dir)});
}

QString Platform::osTag()
{
    return QStringLiteral("win");
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

QString Platform::removeCommandText(const QString &path, const QString &, bool onlyFile)
{
    return (onlyFile ? QStringLiteral("del /f /q \"%1\"") : QStringLiteral("rmdir /s /q \"%1\"")).arg(QDir::toNativeSeparators(path));
}

void Platform::setRemoveCommand(QProcess *process, const QString &path, bool onlyFile)
{
    // rmdir and del do not always set an exit code when something stays (a file in use): check that it is gone.
    const QString p = QDir::toNativeSeparators(path);
    setCmdLine(process, (onlyFile ? QStringLiteral("del /f /q \"%1\"") : QStringLiteral("rmdir /s /q \"%1\"")).arg(p) +
                            QStringLiteral(" & if exist \"%1\" (exit 1) else (exit 0)").arg(p));
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
        // The PATH the process will get when one was set (MCP servers add folders), else QODE's own. findExecutable adds
        // PATHEXT: gradle -> gradle.bat, npx -> npx.cmd.
        const QProcessEnvironment env = process->processEnvironment();
        const QStringList dirs = env.contains(QStringLiteral("PATH"))
                                     ? env.value(QStringLiteral("PATH")).split(QDir::listSeparator(), Qt::SkipEmptyParts)
                                     : QStringList();
        const QString found = QStandardPaths::findExecutable(exe, dirs);
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
    // .\gradlew.bat, .\mvnw.cmd: the file itself, as the user sees it in the folder (cmd and PowerShell would also find
    // it from .\gradlew, but the full name says which script runs).
    const QString file = wrapperFile(dir, name);
    return file.isEmpty() ? QString() : QStringLiteral(".\\") + QFileInfo(file).fileName();
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

QString Platform::pythonCommand()
{
    // python.exe may be the Microsoft Store stub (WindowsApps), which opens the Store; py is only there when Python is.
    return QStandardPaths::findExecutable(QStringLiteral("py")).isEmpty() ? QStringLiteral("python") : QStringLiteral("py");
}

QString Platform::pythonInterpreter()
{
    static QString found; // asking py costs a process start; a found interpreter stays valid while it exists
    if (!found.isEmpty() && QFileInfo::exists(found))
        return found;
    found.clear();
    // 1. The py launcher knows the installed versions (and is never the Store stub).
    if (const QString py = QStandardPaths::findExecutable(QStringLiteral("py")); !py.isEmpty()) {
        QProcess p;
        p.setStandardInputFile(QProcess::nullDevice()); // never wait for an answer (an install manager prompt)
        p.start(py, {QStringLiteral("-3"), QStringLiteral("-c"), QStringLiteral("import sys; print(sys.executable)")});
        if (p.waitForFinished(8000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            const QString exe = QDir::fromNativeSeparators(decodeOutput(p.readAllStandardOutput()).trimmed());
            if (QFileInfo(exe).isFile())
                return found = exe;
        } else {
            killTree(&p);
            p.waitForFinished(1000);
        }
    }
    // 2. python.exe on PATH, except the Store's stubs (App Execution Aliases in WindowsApps), which open the Store.
    for (const char *name : {"python3", "python"}) {
        const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!exe.isEmpty() && !exe.contains(QLatin1String("/WindowsApps/"), Qt::CaseInsensitive))
            return found = exe;
    }
    // 3. The installers' folders: python.org per user / all users, and the install manager's runtimes.
    QStringList candidates;
    const auto addVersions = [&candidates](const QString &dir, const QString &pattern) {
        QDir d(dir);
        QStringList names = d.entryList({pattern}, QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
            const auto version = [](const QString &n) { // Python313 / pythoncore-3.13-64 -> 3.13
                static const QRegularExpression re(QStringLiteral("(\\d)\\.?(\\d+)"));
                const auto m = re.match(n);
                return m.hasMatch() ? m.captured(1).toInt() * 1000 + m.captured(2).toInt() : 0;
            };
            return version(a) > version(b);
        });
        for (const QString &n : std::as_const(names))
            candidates << d.filePath(n) + QStringLiteral("/python.exe");
    };
    const QString local = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    if (!local.isEmpty()) {
        addVersions(local + QStringLiteral("/Programs/Python"), QStringLiteral("Python3*"));
        addVersions(local + QStringLiteral("/Python"), QStringLiteral("pythoncore-3*"));
    }
    for (const QString &root : programFilesRoots())
        addVersions(root, QStringLiteral("Python3*"));
    for (const QString &exe : std::as_const(candidates))
        if (QFileInfo(exe).isFile())
            return found = exe;
    return {};
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

QString Platform::gitBashExecutable()
{
    return gitBash();
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

bool Platform::hasMsvc()
{
    return !vcvars64().isEmpty();
}

QMap<QString, QString> Platform::templateValues()
{
    static const QMap<QString, QString> cached = [] { // the New Project dialog asks on every change
        QMap<QString, QString> v;
        const bool msvc = hasMsvc();
        const auto pick = [msvc](const char *gnu, const char *llvm) {
            if (msvc)
                return QStringLiteral("msvc");
            if (!QStandardPaths::findExecutable(QLatin1String(gnu)).isEmpty())
                return QString::fromLatin1(gnu);
            if (!QStandardPaths::findExecutable(QLatin1String(llvm)).isEmpty())
                return QString::fromLatin1(llvm);
            return QStringLiteral("msvc");
        };
        v.insert(QStringLiteral("cCompiler"), pick("gcc", "clang"));
        v.insert(QStringLiteral("cxxCompiler"), pick("g++", "clang++"));
        QString qmake;
        for (const char *name : {"qmake6", "qmake"})
            if (qmake.isEmpty())
                qmake = QStandardPaths::findExecutable(QLatin1String(name));
        if (qmake.isEmpty())
            qmake = qmakeCandidates().value(0);
        v.insert(QStringLiteral("qmake"), qmake.isEmpty() ? QString() : batPath(qmake));
        v.insert(QStringLiteral("qtKit"), qmake.isEmpty() ? QString() : isMsvcKit(qmake) ? QStringLiteral("msvc") : QStringLiteral("mingw"));
        return v;
    }();
    return cached;
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

QStringList Platform::systemIncludeDirs()
{
    static const QStringList dirs = [] {
        // The highest-versioned subfolder of `parent` ("14.51.36014", "10.0.26100.0", "15.2.0"), empty when none.
        const auto newest = [](const QString &parent) {
            QString best;
            QVersionNumber bestVersion;
            for (const QString &name : QDir(parent).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QVersionNumber v = QVersionNumber::fromString(name);
                if (!v.isNull() && (best.isEmpty() || v > bestVersion)) {
                    best = parent + QLatin1Char('/') + name;
                    bestVersion = v;
                }
            }
            return best;
        };
        QStringList out;
        // MSVC: <VS>\VC\Tools\MSVC\<version>\include, plus the Windows SDK's headers.
        if (const QString bat = vcvars64(); !bat.isEmpty()) {
            const QString vc = QDir::cleanPath(QFileInfo(bat).absolutePath() + QStringLiteral("/../.."));
            if (const QString tools = newest(vc + QStringLiteral("/Tools/MSVC")); !tools.isEmpty())
                out << tools + QStringLiteral("/include");
            const QSettings kits(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"), QSettings::NativeFormat);
            QString root = QDir::fromNativeSeparators(kits.value(QStringLiteral("KitsRoot10")).toString());
            if (root.isEmpty())
                root = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles(x86)", QStringLiteral("C:\\Program Files (x86)"))) +
                       QStringLiteral("/Windows Kits/10");
            if (const QString sdk = newest(QDir::cleanPath(root) + QStringLiteral("/Include")); !sdk.isEmpty())
                for (const char *sub : {"ucrt", "um", "shared", "winrt"})
                    out << sdk + QLatin1Char('/') + QLatin1String(sub);
        }
        // MinGW-w64 from MSYS2: <env>\include\c++\<version> (+ its target folder) and <env>\include.
        if (out.isEmpty()) {
            QString msys = QDir::fromNativeSeparators(qEnvironmentVariable("MSYS2_ROOT"));
            if (msys.isEmpty())
                msys = QStringLiteral("C:/msys64");
            for (const char *env : {"ucrt64", "clang64", "mingw64"}) {
                const QString inc = msys + QLatin1Char('/') + QLatin1String(env) + QStringLiteral("/include");
                if (!QFileInfo(inc).isDir())
                    continue;
                if (const QString cxx = newest(inc + QStringLiteral("/c++")); !cxx.isEmpty())
                    out << cxx << cxx + QStringLiteral("/x86_64-w64-mingw32");
                out << inc;
                break;
            }
        }
        QStringList existing;
        for (const QString &d : std::as_const(out))
            if (QFileInfo(d).isDir())
                existing << d;
        return existing;
    }();
    return dirs;
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
    if (!appData.isEmpty())
        add(appData + QStringLiteral("\\fnm\\aliases\\default")); // fnm's default Node.js (node.exe only)
    add(QDir::homePath() + QStringLiteral("/scoop/shims"));
    return out;
}

QStringList Platform::languageServerDirs()
{
    QStringList out;
    for (const QString &root : programFilesRoots())
        if (const QString dir = root + QStringLiteral("/LLVM/bin"); QFileInfo(dir).isDir())
            out << dir;
    QString msys = qEnvironmentVariable("MSYS2_ROOT");
    if (msys.isEmpty())
        msys = QStringLiteral("C:/msys64");
    for (const char *env : {"ucrt64", "clang64", "mingw64"})
        if (const QString dir = QDir::fromNativeSeparators(msys) + QLatin1Char('/') + QLatin1String(env) + QStringLiteral("/bin"); QFileInfo(dir).isDir())
            out << dir;
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

QList<Platform::ChatShell> Platform::chatShells()
{
    QList<ChatShell> out;
    const QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
    if (!pwsh.isEmpty()) {
        out.append({QStringLiteral("powershell"), QStringLiteral("PowerShell 7"), QStringLiteral("powershell"), pwsh});
    } else {
        const QString system = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")) + QStringLiteral("\\System32");
        out.append({QStringLiteral("powershell"), QStringLiteral("Windows PowerShell 5.1"), QStringLiteral("powershell"),
                    system + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe")});
    }
    const QString bash = gitBash();
    if (!bash.isEmpty())
        out.append({QStringLiteral("gitbash"), QStringLiteral("Git Bash"), QStringLiteral("bash"), QDir::toNativeSeparators(bash)});
    return out;
}

Platform::ChatShell Platform::chatShell(const QString &id)
{
    const QList<ChatShell> all = chatShells();
    for (const ChatShell &s : all)
        if (s.id == id)
            return s;
    return all.constFirst();
}

void Platform::setChatShellCommand(QProcess *process, const ChatShell &shell, const QString &command)
{
    process->setProgram(shell.program);
    if (shell.dialect != QLatin1String("powershell")) {
        process->setArguments({QStringLiteral("-c"), command});
        return;
    }
    // The command travels in an environment variable (a file when it is too long for one) and a fixed -Command line
    // runs it: no quoting, and errors stay text (with -EncodedCommand Windows PowerShell writes them to stderr as
    // CLIXML). The bootstrap makes PowerShell write UTF-8 (a pipe gets the OEM code page otherwise) and hides progress
    // bars; the lines after the command give bash's exit code (-Command alone only knows 0 and 1). A parse error shows
    // the command's own line numbers.
    const QString script = command
                           + QStringLiteral("\nif ($?) { exit 0 }\n"
                                            "if ($global:LASTEXITCODE) { exit $global:LASTEXITCODE }\n"
                                            "exit 1\n");
    static const QString bootstrap = QStringLiteral(
        "$ProgressPreference = 'SilentlyContinue'; "
        "try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch {}; "
        "$OutputEncoding = [System.Text.UTF8Encoding]::new($false); "
        "$__qf = $env:QODE_CHAT_SCRIPT_FILE; "
        "$__qs = if ($__qf) { [IO.File]::ReadAllText($__qf) } else { $env:QODE_CHAT_SCRIPT }; "
        "Remove-Item Env:QODE_CHAT_SCRIPT* -ErrorAction SilentlyContinue; "
        "try { $__qb = [ScriptBlock]::Create($__qs) } catch { [Console]::Error.WriteLine($_.Exception.InnerException.Message); exit 1 }; "
        "$global:LASTEXITCODE = 0; "
        ". $__qb");
    QProcessEnvironment env = process->processEnvironment();
    if (env.isEmpty())
        env = QProcessEnvironment::systemEnvironment();
    if (script.size() < 30000) { // an environment variable holds 32767 characters
        env.insert(QStringLiteral("QODE_CHAT_SCRIPT"), script);
    } else {
        QString path;
        {
            QTemporaryFile file(QDir::tempPath() + QStringLiteral("/qode-chat-XXXXXX.ps1"));
            file.setAutoRemove(false);
            if (file.open()) {
                file.write(script.toUtf8());
                path = file.fileName();
            }
        } // closed for good here: an open QTemporaryFile cannot be read by another process
        QObject::connect(process, &QObject::destroyed, [path] { QFile::remove(path); });
        env.insert(QStringLiteral("QODE_CHAT_SCRIPT_FILE"), QDir::toNativeSeparators(path));
    }
    process->setProcessEnvironment(env);
    const QStringList args{QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                           QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), // npm.ps1 and a project's .ps1 run too
                           QStringLiteral("-Command"), bootstrap};
    process->setArguments(args);
}

QString Platform::fromShellPath(const QString &path)
{
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    static const QRegularExpression drive(QStringLiteral("^/(?:cygdrive/|mnt/)?([a-zA-Z])(?=/|$)"));
    const QRegularExpressionMatch m = drive.match(p);
    if (m.hasMatch()) {
        const QString root = m.captured(1).toUpper() + QStringLiteral(":/");
        if (QFileInfo(root).exists())
            p = root + p.mid(m.capturedEnd() + 1);
    }
    return p;
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
