#include "NpmInstaller.h"

#include "LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

QString NpmInstaller::installRoot()
{
    return LspServers::managedDir(QStringLiteral("web"));
}

QStringList NpmInstaller::packages()
{
    // typescript@6: version 7 is the native port, which has no tsserver.js for typescript-language-server to drive.
    return {QStringLiteral("typescript@6"), QStringLiteral("typescript-language-server"), QStringLiteral("vscode-langservers-extracted")};
}

QString NpmInstaller::commandLine()
{
    QString root = installRoot();
    const QString home = QDir::homePath();
    if (root.startsWith(home + QLatin1Char('/')))
        root = QStringLiteral("~") + root.mid(home.size());
    return QStringLiteral("npm install --prefix %1 %2").arg(root, packages().join(QLatin1Char(' ')));
}

bool NpmInstaller::isInstalled()
{
    for (const LspServerSpec &s : LspServers::all())
        if (s.installer == LspServerSpec::Installer::Npm && LspServers::managedExecutable(s).isEmpty())
            return false;
    return true;
}

NpmInstaller::NpmInstaller(QObject *parent) : QObject(parent) {}

NpmInstaller::~NpmInstaller()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
    }
}

void NpmInstaller::start()
{
    if (m_proc)
        return;
    const QString node = LspServers::nodeExecutable();
    const QString npm = LspServers::npmExecutable();
    if (node.isEmpty() || npm.isEmpty()) {
        emit failed(tr("Node.js and npm were not found. The web language servers are Node.js programs, so install Node.js "
                       "first (Debian / Ubuntu: sudo apt install nodejs npm; Fedora: sudo dnf install nodejs; "
                       "Arch: sudo pacman -S nodejs npm; or https://nodejs.org) and try again."));
        return;
    }
    if (!QDir().mkpath(installRoot())) {
        emit failed(tr("Could not create %1").arg(installRoot()));
        return;
    }
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    // npm is a "#!/usr/bin/env node" script: make sure that finds the node it was found next to.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"), QFileInfo(node).absolutePath() + QLatin1Char(':') + env.value(QStringLiteral("PATH")));
    m_proc->setProcessEnvironment(env);
    connect(m_proc, &QProcess::readyRead, this, [this] { emit output(QString::fromLocal8Bit(m_proc->readAll())); });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            abort(tr("Could not start npm"));
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        emit output(QString::fromLocal8Bit(m_proc->readAll()));
        m_proc->deleteLater();
        m_proc = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            emit failed(tr("npm failed (exit code %1). See the output above.").arg(code));
            return;
        }
        for (const LspServerSpec &s : LspServers::all())
            if (s.installer == LspServerSpec::Installer::Npm && LspServers::managedExecutable(s).isEmpty()) {
                emit failed(tr("npm finished, but %1 is missing from %2.").arg(s.managedBinary, installRoot()));
                return;
            }
        emit finished();
    });
    QStringList args{QStringLiteral("install"), QStringLiteral("--prefix"), installRoot(), QStringLiteral("--no-audit"),
                     QStringLiteral("--no-fund"), QStringLiteral("--no-progress")};
    args << packages();
    m_proc->start(npm, args);
}

void NpmInstaller::cancel()
{
    if (!m_proc)
        return;
    m_proc->disconnect(this);
    m_proc->kill();
    m_proc->waitForFinished(500);
    m_proc->deleteLater();
    m_proc = nullptr;
    emit failed(tr("Cancelled."));
}

void NpmInstaller::abort(const QString &error)
{
    if (m_proc) {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    emit failed(error);
}
