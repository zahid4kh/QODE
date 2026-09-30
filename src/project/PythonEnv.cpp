#include "PythonEnv.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace PythonEnv {

static bool isVenv(const QDir &root, const QString &name)
{
    return QFileInfo::exists(root.filePath(name + QStringLiteral("/pyvenv.cfg")))
        && QFileInfo::exists(root.filePath(name + QStringLiteral("/bin/activate")));
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

QString activationCommand(const QString &projectRoot, const QString &shellName)
{
    const QString venv = findVenv(projectRoot);
    if (venv.isEmpty())
        return {};
    QString script = QStringLiteral("activate");
    if (shellName == QLatin1String("fish"))
        script = QStringLiteral("activate.fish");
    else if (shellName == QLatin1String("csh") || shellName == QLatin1String("tcsh"))
        script = QStringLiteral("activate.csh");
    const QString rel = venv + QStringLiteral("/bin/") + script;
    if (!QFileInfo::exists(QDir(projectRoot).filePath(rel)))
        return {};
    // Single-quote so names with spaces or shell metacharacters survive.
    QString quoted = QLatin1String("./") + rel;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("source '%1'").arg(quoted);
}

} // namespace PythonEnv
