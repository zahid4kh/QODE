#include "PackageManager.h"

#include "lsp/LspServers.h"
#include "project/PythonEnv.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace {
QProcessEnvironment pythonEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : PythonTools::environment())
        env.insert(kv.section(QLatin1Char('='), 0, 0), kv.section(QLatin1Char('='), 1));
    env.remove(QStringLiteral("VIRTUAL_ENV")); // the tools are pointed at the environment explicitly
    return env;
}
} // namespace

PackageManager::PackageManager(QObject *parent) : QObject(parent)
{
    m_useUv = !LspServers::uvExecutable().isEmpty();
}

PackageManager::~PackageManager()
{
    for (QProcess *p : {m_op, m_list})
        if (p) {
            p->disconnect(this);
            p->kill();
            p->waitForFinished(300);
        }
}

void PackageManager::setEnvironment(const QString &projectRoot, const QString &venv)
{
    m_root = projectRoot;
    m_venv = venv;
    m_useUv = !LspServers::uvExecutable().isEmpty();
}

QString PackageManager::python() const
{
    return PythonEnv::venvPython(m_root, m_venv);
}

void PackageManager::refresh()
{
    if (python().isEmpty()) {
        emit listFailed(tr("This environment has no Python interpreter (%1/bin/python is missing).").arg(m_venv));
        return;
    }
    listInto(false);
}

// Packages come from stdout as JSON; stderr (notices, warnings) is kept apart so it cannot corrupt the document.
void PackageManager::listInto(bool outdated)
{
    if (m_list) {
        m_list->disconnect(this);
        m_list->kill();
        m_list->deleteLater();
        m_list = nullptr;
    }
    const PythonTools::Command cmd = PythonTools::list(python(), outdated, m_useUv);
    m_list = new QProcess(this);
    m_list->setProcessEnvironment(pythonEnvironment());
    m_list->setWorkingDirectory(m_root);
    QProcess *proc = m_list;
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || proc != m_list)
            return;
        m_list->deleteLater();
        m_list = nullptr;
        emit listFailed(tr("Could not start %1").arg(tool()));
    });
    connect(proc, &QProcess::finished, this, [this, proc, outdated](int code, QProcess::ExitStatus status) {
        if (proc != m_list)
            return;
        const QByteArray out = proc->readAllStandardOutput();
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        proc->deleteLater();
        m_list = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            if (!outdated) { // an unreachable package index only costs the "Latest" column
                QString message = err.isEmpty() ? tr("Listing the packages failed (exit code %1).").arg(code) : err;
                if (err.contains(QLatin1String("No module named pip")))
                    message = tr("This environment has no pip. Install uv, or run python -m ensurepip --upgrade inside it.");
                emit listFailed(message);
            }
            return;
        }
        const QList<PythonTools::Package> packages = PythonTools::parseList(out);
        if (outdated) {
            emit outdatedListed(packages);
        } else {
            emit installedListed(packages);
            listInto(true);
        }
    });
    proc->start(cmd.program, cmd.arguments);
}

void PackageManager::install(const QStringList &specs, bool upgrade)
{
    if (specs.isEmpty() || python().isEmpty())
        return;
    run(PythonTools::install(python(), specs, upgrade, m_useUv),
        upgrade ? tr("Upgrading %1").arg(specs.join(QStringLiteral(", "))) : tr("Installing %1").arg(specs.join(QStringLiteral(", "))));
}

void PackageManager::uninstall(const QStringList &names)
{
    if (names.isEmpty() || python().isEmpty())
        return;
    run(PythonTools::uninstall(python(), names, m_useUv), tr("Uninstalling %1").arg(names.join(QStringLiteral(", "))));
}

void PackageManager::installFromFile(const QString &file)
{
    if (python().isEmpty())
        return;
    const QFileInfo fi(file);
    const QString name = fi.fileName().toLower();
    if (name == QLatin1String("pyproject.toml") || name == QLatin1String("setup.py") || name == QLatin1String("setup.cfg"))
        run(PythonTools::installProject(python(), fi.absolutePath(), m_useUv), tr("Installing the project in %1").arg(fi.dir().dirName()));
    else
        run(PythonTools::installRequirements(python(), fi.absoluteFilePath(), m_useUv), tr("Installing from %1").arg(fi.fileName()));
}

void PackageManager::createVenv(const QString &name)
{
    const QString dir = QDir(m_root).filePath(name);
    run(PythonTools::createVenv(dir, m_useUv, LspServers::pythonExecutable()), tr("Creating %1").arg(name));
}

void PackageManager::run(const PythonTools::Command &cmd, const QString &title)
{
    if (m_op)
        return;
    if (cmd.program.isEmpty()) {
        emit operationStarted(title);
        emit operationFinished(false, title, tr("Python 3 was not found. Install python3 (and python3-venv) or uv and try again."));
        return;
    }
    m_title = title;
    m_collected.clear();
    emit operationStarted(title);
    emit output(QStringLiteral("$ ") + cmd.display);
    m_op = new QProcess(this);
    m_op->setProcessChannelMode(QProcess::MergedChannels);
    m_op->setProcessEnvironment(pythonEnvironment());
    m_op->setWorkingDirectory(m_root);
    QProcess *proc = m_op;
    connect(proc, &QProcess::readyRead, this, [this, proc] {
        const QString text = QString::fromLocal8Bit(proc->readAll());
        m_collected += text;
        emit output(text);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || proc != m_op)
            return;
        m_op->deleteLater();
        m_op = nullptr;
        emit operationFinished(false, m_title, tr("Could not start %1").arg(tool()));
    });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        if (proc != m_op)
            return;
        const QString tail = QString::fromLocal8Bit(proc->readAll());
        m_collected += tail;
        if (!tail.isEmpty())
            emit output(tail);
        proc->deleteLater();
        m_op = nullptr;
        if (status == QProcess::NormalExit && code == 0) {
            emit operationFinished(true, m_title, QString());
            return;
        }
        QString error = tr("The command failed (exit code %1). See the log for details.").arg(code);
        if (m_collected.contains(QLatin1String("No matching distribution")) || m_collected.contains(QLatin1String("No solution found")) ||
            m_collected.contains(QLatin1String("Could not find a version")) || m_collected.contains(QLatin1String("not found in the package registry")))
            error = tr("No such package or version. Check the name and spelling.");
        else if (m_collected.contains(QLatin1String("No module named pip")))
            error = tr("This environment has no pip. Install uv, or run python -m ensurepip --upgrade inside it.");
        else if (m_collected.contains(QLatin1String("ensurepip")) || m_collected.contains(QLatin1String("python3-venv")))
            error = tr("Python could not create a virtual environment because its venv module is missing. On Debian / Ubuntu run "
                       "sudo apt install python3-venv.");
        else if (m_collected.contains(QLatin1String("Temporary failure")) || m_collected.contains(QLatin1String("Connection")) ||
                 m_collected.contains(QLatin1String("Failed to fetch")))
            error = tr("The package index could not be reached. Check your internet connection.");
        emit operationFinished(false, m_title, error);
    });
    proc->start(cmd.program, cmd.arguments);
}

void PackageManager::cancel()
{
    if (!m_op)
        return;
    QProcess *proc = m_op;
    m_op = nullptr;
    proc->disconnect(this);
    proc->kill();
    proc->waitForFinished(500);
    proc->deleteLater();
    emit operationFinished(false, m_title, tr("Cancelled."));
}
