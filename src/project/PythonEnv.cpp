#include "PythonEnv.h"

#include "platform/Platform.h"
#include "settings/SettingsManager.h"
#include "terminal/ShellSyntax.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace PythonEnv {

static bool isVenv(const QDir &root, const QString &name)
{
    return QFileInfo::exists(root.filePath(name + QStringLiteral("/pyvenv.cfg")))
        && QFileInfo::exists(root.filePath(name + QLatin1Char('/') + Platform::venvBinDir() + QStringLiteral("/activate")));
}

QString findVenv(const QString &projectRoot)
{
    const QDir root(projectRoot);
    if (projectRoot.isEmpty() || !root.exists())
        return {};
    // Conventional names win; any other folder holding a pyvenv.cfg is found by the scan.
    for (const QString &name : {QStringLiteral(".venv"), QStringLiteral("venv"), QStringLiteral("env"),
                                QStringLiteral(".env")})
        if (isVenv(root, name))
            return name;
    const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString &name : dirs)
        if (isVenv(root, name))
            return name;
    return {};
}

QStringList findVenvs(const QString &projectRoot)
{
    const QDir root(projectRoot);
    QStringList out;
    if (projectRoot.isEmpty() || !root.exists())
        return out;
    for (const QString &name : {QStringLiteral(".venv"), QStringLiteral("venv"), QStringLiteral("env"),
                                QStringLiteral(".env")})
        if (isVenv(root, name))
            out << name;
    const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString &name : dirs)
        if (!out.contains(name) && isVenv(root, name))
            out << name;
    return out;
}

QString activeVenv(const QString &projectRoot)
{
    const QString chosen = SettingsManager::instance().pythonVenv();
    if (!chosen.isEmpty() && !projectRoot.isEmpty() && isVenv(QDir(projectRoot), chosen))
        return chosen;
    return findVenv(projectRoot);
}

QString venvPython(const QString &projectRoot, const QString &venv)
{
    if (venv.isEmpty())
        return {};
    for (const char *name : {"python", "python3"}) {
        const QString exe = Platform::venvProgram(QDir(projectRoot).filePath(venv), QString::fromLatin1(name));
        if (!exe.isEmpty())
            return exe;
    }
    return {};
}

QString venvVersion(const QString &projectRoot, const QString &venv)
{
    QFile f(QDir(projectRoot).filePath(venv + QStringLiteral("/pyvenv.cfg")));
    if (venv.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    // venv writes "version = 3.12.3", uv writes "version_info = 3.12.3" (virtualenv: "version_info" too).
    static const QRegularExpression re(QStringLiteral("^version(?:_info)?\\s*=\\s*(\\d+\\.\\d+(?:\\.\\d+)?)"),
                                       QRegularExpression::MultilineOption);
    return re.match(QString::fromUtf8(f.readAll())).captured(1);
}

QString activationCommand(const QString &projectRoot, const QString &shellName)
{
    const QString venv = activeVenv(projectRoot);
    if (venv.isEmpty())
        return {};
    const ShellSyntax::Dialect dialect = ShellSyntax::dialectOf(shellName);
    const QString abs = QDir::toNativeSeparators(QDir(projectRoot).filePath(venv));
    const QString scripts = abs + QDir::separator() + Platform::venvBinDir();
    if (dialect == ShellSyntax::Dialect::PowerShell) {
        // What Activate.ps1 does that matters, without running a script (the default execution policy forbids it).
        return QStringLiteral("$env:VIRTUAL_ENV = %1; $env:PATH = %2 + ';' + $env:PATH")
            .arg(ShellSyntax::quote(dialect, abs), ShellSyntax::quote(dialect, scripts));
    }
    if (dialect == ShellSyntax::Dialect::Cmd) {
        const QString bat = scripts + QStringLiteral("\\activate.bat");
        return QFileInfo::exists(bat) ? ShellSyntax::quote(dialect, bat) : QString();
    }
    QString script = QStringLiteral("activate");
    if (shellName == QLatin1String("fish"))
        script = QStringLiteral("activate.fish");
    else if (shellName == QLatin1String("csh") || shellName == QLatin1String("tcsh"))
        script = QStringLiteral("activate.csh");
    const QString rel = venv + QLatin1Char('/') + Platform::venvBinDir() + QLatin1Char('/') + script;
    if (!QFileInfo::exists(QDir(projectRoot).filePath(rel)))
        return {};
    // Single-quote so names with spaces or shell metacharacters survive.
    QString quoted = QLatin1String("./") + rel;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("source '%1'").arg(quoted);
}

} // namespace PythonEnv
