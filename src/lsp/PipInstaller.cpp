#include "PipInstaller.h"

#include "LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

QString PipInstaller::installRoot()
{
    return LspServers::managedDir(QStringLiteral("python"));
}

QStringList PipInstaller::packages()
{
    return {QStringLiteral("basedpyright"), QStringLiteral("ruff")};
}

bool PipInstaller::usesUv()
{
    return !LspServers::uvExecutable().isEmpty();
}

bool PipInstaller::isInstalled()
{
    for (const LspServerSpec &s : LspServers::all())
        if (s.installer == LspServerSpec::Installer::Pip && LspServers::managedExecutable(s).isEmpty())
            return false;
    return true;
}

QString PipInstaller::installedVersion()
{
    const QString exe = LspServers::managedExecutable(*LspServers::byId(QStringLiteral("python")));
    if (exe.isEmpty())
        return {};
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(installRoot() + QStringLiteral("/bin/python"),
            {QStringLiteral("-c"), QStringLiteral("import importlib.metadata as m; print(m.version('basedpyright'))")});
    if (!p.waitForFinished(4000)) {
        p.kill();
        return {};
    }
    return QString::fromLocal8Bit(p.readAll()).trimmed();
}

QList<PythonTools::Command> PipInstaller::buildSteps(bool useUv) const
{
    QList<PythonTools::Command> steps;
    const QString python = installRoot() + QStringLiteral("/bin/python");
    if (!QFileInfo(python).isExecutable())
        steps << PythonTools::createVenv(installRoot(), useUv, LspServers::pythonExecutable());
    steps << PythonTools::install(python, packages(), true, useUv);
    return steps;
}

QStringList PipInstaller::plannedCommands()
{
    PipInstaller probe;
    QStringList out;
    for (const PythonTools::Command &c : probe.buildSteps(usesUv()))
        out << c.display;
    return out;
}

PipInstaller::PipInstaller(QObject *parent) : QObject(parent) {}

PipInstaller::~PipInstaller()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
    }
}

void PipInstaller::start()
{
    if (m_proc)
        return;
    m_useUv = usesUv();
    if (!m_useUv && LspServers::pythonExecutable().isEmpty()) {
        emit failed(tr("Python 3 was not found. The Python language server is installed into a virtual environment, so install "
                       "Python first (Debian / Ubuntu: sudo apt install python3 python3-venv; Fedora: sudo dnf install python3; "
                       "Arch: sudo pacman -S python; or https://www.python.org) and try again."));
        return;
    }
    if (!QDir().mkpath(QFileInfo(installRoot()).absolutePath())) {
        emit failed(tr("Could not create %1").arg(QFileInfo(installRoot()).absolutePath()));
        return;
    }
    m_retriedWithPip = false;
    m_createdVenv = !QFileInfo(installRoot() + QStringLiteral("/bin/python")).isExecutable();
    m_steps = buildSteps(m_useUv);
    m_next = 0;
    runNext();
}

void PipInstaller::runNext()
{
    if (m_next >= m_steps.size()) {
        for (const LspServerSpec &s : LspServers::all())
            if (s.installer == LspServerSpec::Installer::Pip && LspServers::managedExecutable(s).isEmpty()) {
                emit failed(tr("The installation finished, but %1 is missing from %2.").arg(s.managedBinary, installRoot()));
                return;
            }
        emit finished();
        return;
    }
    const PythonTools::Command &step = m_steps.at(m_next++);
    emit output(QStringLiteral("$ ") + step.display);
    m_collected.clear();
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : PythonTools::environment())
        env.insert(kv.section(QLatin1Char('='), 0, 0), kv.section(QLatin1Char('='), 1));
    env.remove(QStringLiteral("VIRTUAL_ENV")); // a venv active in QODE's own shell must not leak into the new one
    m_proc->setProcessEnvironment(env);
    connect(m_proc, &QProcess::readyRead, this, [this] {
        const QString text = QString::fromLocal8Bit(m_proc->readAll());
        m_collected += text;
        emit output(text);
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this, step](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            abort(tr("Could not start %1").arg(QFileInfo(step.program).fileName()));
    });
    connect(m_proc, &QProcess::finished, this, &PipInstaller::stepFinished);
    m_proc->start(step.program, step.arguments);
}

void PipInstaller::stepFinished(int code, QProcess::ExitStatus status)
{
    const QString text = QString::fromLocal8Bit(m_proc->readAll());
    m_collected += text;
    emit output(text);
    m_proc->deleteLater();
    m_proc = nullptr;
    if (status == QProcess::NormalExit && code == 0) {
        runNext();
        return;
    }
    fail(tr("The command failed (exit code %1). See the output above.").arg(code));
}

// A failed uv run is retried once with python3 + pip; everything else ends here with a hint where one is known.
void PipInstaller::fail(const QString &error)
{
    if (m_useUv && !m_retriedWithPip && !LspServers::pythonExecutable().isEmpty()) {
        m_retriedWithPip = true;
        m_useUv = false;
        emit output(tr("uv did not succeed — trying again with python3 and pip."));
        if (m_createdVenv)
            QDir(installRoot()).removeRecursively();
        m_createdVenv = !QFileInfo(installRoot() + QStringLiteral("/bin/python")).isExecutable();
        m_steps = buildSteps(false);
        m_next = 0;
        runNext();
        return;
    }
    if (m_createdVenv)
        QDir(installRoot()).removeRecursively(); // leave nothing half-built behind
    QString message = error;
    if (m_collected.contains(QLatin1String("ensurepip")) || m_collected.contains(QLatin1String("python3-venv")))
        message = tr("Python could not create a virtual environment because its venv module is missing. On Debian / Ubuntu run "
                     "sudo apt install python3-venv, then try again.");
    else if (m_collected.contains(QLatin1String("externally-managed-environment")))
        message = tr("Python refused the installation (externally-managed environment). Install python3-venv or uv and try again.");
    else if (m_collected.contains(QLatin1String("Could not find a version")) || m_collected.contains(QLatin1String("Failed to fetch")) ||
             m_collected.contains(QLatin1String("Connection")) || m_collected.contains(QLatin1String("Temporary failure")))
        message = tr("The packages could not be downloaded. Check your internet connection and try again.");
    emit failed(message);
}

void PipInstaller::cancel()
{
    if (!m_proc)
        return;
    m_proc->disconnect(this);
    m_proc->kill();
    m_proc->waitForFinished(500);
    m_proc->deleteLater();
    m_proc = nullptr;
    if (m_createdVenv)
        QDir(installRoot()).removeRecursively();
    emit failed(tr("Cancelled."));
}

void PipInstaller::abort(const QString &error)
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    fail(error);
}
