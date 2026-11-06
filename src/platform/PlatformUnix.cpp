#include "platform/Platform.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QProcess>

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

QStringList Platform::extraBinDirs()
{
    return {};
}

void Platform::restartSelf()
{
    QProcess::startDetached(QStringLiteral("/bin/sh"),
                            {QStringLiteral("-c"), QStringLiteral("sleep 1; exec \"$0\""), QCoreApplication::applicationFilePath()});
}
