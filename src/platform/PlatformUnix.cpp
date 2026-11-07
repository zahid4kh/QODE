#include "platform/Platform.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <csignal>
#include <cstdio>
#include <unistd.h>

bool Platform::runIfHelper(int, char *[])
{
    return false;
}

void Platform::initProcess()
{
    std::signal(SIGPIPE, SIG_IGN);
}

QString Platform::decodeOutput(const QByteArray &bytes)
{
    return QString::fromLocal8Bit(bytes);
}

qint64 Platform::msBeforeMain()
{
    QFile stat(QStringLiteral("/proc/self/stat")), uptime(QStringLiteral("/proc/uptime"));
    if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray s = stat.readAll();
    // Fields after the command name "(…)", which may hold spaces; starttime is field 22, the 20th after it.
    const QList<QByteArray> fields = s.mid(s.lastIndexOf(')') + 2).split(' ');
    const long ticks = sysconf(_SC_CLK_TCK);
    if (fields.size() < 20 || ticks <= 0)
        return -1;
    const double started = fields.at(19).toDouble() / ticks;
    const double now = uptime.readAll().split(' ').value(0).toDouble();
    return now > 0 && now >= started ? qint64((now - started) * 1000) : -1;
}

QString Platform::userKey()
{
    return QString::number(::getuid());
}

bool Platform::replaceSymlink(const QString &target, const QString &link)
{
    const QString tmp = link + QStringLiteral(".new");
    ::unlink(QFile::encodeName(tmp).constData());
    if (::symlink(QFile::encodeName(target).constData(), QFile::encodeName(tmp).constData()) != 0)
        return false;
    if (std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(link).constData()) != 0) {
        ::unlink(QFile::encodeName(tmp).constData());
        return false;
    }
    return true;
}

bool Platform::removeTree(const QString &path)
{
    return QDir(path).removeRecursively();
}

QString Platform::removeCommandText(const QString &, const QString &shownPath, bool onlyFile)
{
    return (onlyFile ? QStringLiteral("rm %1") : QStringLiteral("rm -rf %1")).arg(shownPath);
}

void Platform::setRemoveCommand(QProcess *process, const QString &path, bool onlyFile)
{
    process->setProgram(QStringLiteral("rm"));
    process->setArguments(onlyFile ? QStringList{QStringLiteral("--"), path} : QStringList{QStringLiteral("-rf"), QStringLiteral("--"), path});
}

bool Platform::setCurrentVersion(const QString &root, const QString &version)
{
    return replaceSymlink(version, root + QStringLiteral("/current"));
}

QString Platform::currentVersion(const QString &root)
{
    const QFileInfo cur(root + QStringLiteral("/current"));
    return cur.isSymLink() ? QFileInfo(cur.symLinkTarget()).fileName() : QString();
}

QString Platform::commandLinkFile(const QString &link)
{
    return link;
}

bool Platform::writeCommandLink(const QString &target, const QString &file)
{
    return replaceSymlink(target, file);
}

QString Platform::commandLinkTarget(const QString &file)
{
    const QFileInfo fi(file);
    return fi.isSymLink() ? fi.symLinkTarget() : QString();
}

void Platform::setUnpackCommand(QProcess *process, const QString &archive, const QString &dir)
{
    process->setProgram(QStringLiteral("tar"));
    process->setArguments({QStringLiteral("-xzf"), archive, QStringLiteral("-C"), dir});
}

QString Platform::osTag()
{
    return QStringLiteral("linux");
}

void Platform::setShellCommand(QProcess *process, const QString &command)
{
    process->setProgram(QStringLiteral("/bin/sh"));
    process->setArguments({QStringLiteral("-c"), command});
}

void Platform::setCommand(QProcess *process, const QString &program, const QStringList &args)
{
    process->setProgram(program);
    process->setArguments(args);
}

QString Platform::wrapperCommand(const QString &dir, const QString &name)
{
    const QFileInfo fi(dir + QLatin1Char('/') + name);
    if (!fi.exists())
        return {};
    return fi.isExecutable() ? QStringLiteral("./") + name : QStringLiteral("sh ") + name;
}

QString Platform::wrapperFile(const QString &dir, const QString &name)
{
    const QString path = dir + QLatin1Char('/') + name;
    return QFileInfo(path).isExecutable() ? path : QString();
}

bool Platform::wrapperNeedsExecBit(const QString &dir, const QString &name)
{
    const QFileInfo fi(dir + QLatin1Char('/') + name);
    return fi.exists() && !fi.isExecutable();
}

QString Platform::venvBinDir()
{
    return QStringLiteral("bin");
}

QString Platform::venvProgram(const QString &venv, const QString &name)
{
    const QFileInfo fi(venv + QStringLiteral("/bin/") + name);
    return fi.exists() && fi.isExecutable() ? fi.absoluteFilePath() : QString();
}

bool Platform::hasExecBit()
{
    return true;
}

QString Platform::invalidFileName(const QString &)
{
    return {};
}

bool Platform::canMakeSymlinks()
{
    return true;
}

bool Platform::watchesBlockRenames()
{
    return false;
}

int Platform::busyFileRetries()
{
    return 0;
}

bool Platform::hiddenByOs(const QFileInfo &)
{
    return false;
}

QString Platform::legacyEncoding()
{
    return QStringLiteral("Latin-1");
}

QString Platform::pathKey(const QString &path)
{
    return path;
}

bool Platform::samePath(const QString &a, const QString &b)
{
    return a == b;
}

bool Platform::isUnder(const QString &path, const QString &root)
{
    if (root.isEmpty())
        return false;
    return path.startsWith(root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'));
}

QString Platform::uriToPath(const QString &uri)
{
    return QUrl(uri).toLocalFile();
}

QString Platform::pathToUri(const QString &path)
{
    return QUrl::fromLocalFile(path).toString();
}

QString Platform::exeSuffix()
{
    return {};
}

QString Platform::pythonCommand()
{
    return QStringLiteral("python3");
}

QString Platform::pythonInterpreter()
{
    for (const char *name : {"python3", "python"}) {
        const QString onPath = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!onPath.isEmpty())
            return onPath;
    }
    for (const char *dir : {"/usr/bin", "/usr/local/bin", "/bin"}) {
        const QFileInfo fi(QString::fromLatin1(dir) + QStringLiteral("/python3"));
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return {};
}

QString Platform::bashProgram()
{
    return QStringLiteral("bash");
}

QString Platform::gitBashExecutable()
{
    return {};
}

QStringList Platform::qmakeCandidates()
{
    return {};
}

QStringList Platform::qmakeScopes(const QString &)
{
    return {QStringLiteral("unix"),         QStringLiteral("linux"), QStringLiteral("linux-g++"),
            QStringLiteral("linux-g++-64"), QStringLiteral("gcc"),   QStringLiteral("posix")};
}

QString Platform::qmakeRunCommand(const QString &qmake, const QString &, const QString &proRel, const QString &exe)
{
    // A word for sh, quoted only when it needs it.
    const auto word = [](const QString &s) {
        static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_./+=:@%-]+$"));
        if (plain.match(s).hasMatch())
            return s;
        QString q = s;
        q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + q + QLatin1Char('\'');
    };
    const QString name = QFileInfo(qmake).fileName();
    const QString run = QDir::isAbsolutePath(exe) ? word(exe) : QStringLiteral("./") + word(exe);
    return QStringLiteral("cd {project} && mkdir -p build && cd build && %1 ../%2 && make -j$(nproc) && %3")
        .arg(name.isEmpty() ? QStringLiteral("qmake6") : name, word(proRel), run);
}

void Platform::killTree(QProcess *process)
{
    if (process)
        process->kill();
}

QString Platform::realPath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

QList<Platform::JdkPlace> Platform::jdkFolders()
{
    return {};
}

QList<Platform::JdkPlace> Platform::jdkHomes()
{
    return {};
}

QString Platform::gitExecutable()
{
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}

QStringList Platform::extraBinDirs()
{
    return {};
}

QStringList Platform::languageServerDirs()
{
    return {};
}

QList<Platform::Shell> Platform::terminalShells()
{
    return {};
}

QString Platform::processCwd(qint64 pid)
{
    return pid > 0 ? QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget() : QString();
}

bool Platform::shellInForeground(qint64 pid)
{
    if (pid <= 0)
        return false;
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    // "pid (comm) state ppid pgrp session tty_nr tpgid ...": comm may hold spaces, so split after the last ')'.
    const QByteArray stat = f.readAll();
    const QList<QByteArray> fields = stat.mid(stat.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 5 && fields.at(2).toLongLong() == fields.at(5).toLongLong();
}

void Platform::restartSelf()
{
    QProcess::startDetached(QStringLiteral("/bin/sh"),
                            {QStringLiteral("-c"), QStringLiteral("sleep 1; exec \"$0\""), QCoreApplication::applicationFilePath()});
}
