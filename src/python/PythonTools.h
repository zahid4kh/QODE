#pragma once

#include <QString>
#include <QStringList>

// The command lines QODE runs to create virtual environments and to add, remove and list packages. uv is used when
// it is installed (`useUv`), plain pip otherwise; both always target one explicit interpreter (the environment's own
// bin/python), so nothing depends on a shell having activated the environment and system Python is never touched.
// Qt Core only.
namespace PythonTools {

struct Command {
    QString program;
    QStringList arguments;
    QString display; // what the user sees in a log: "pip install requests"
};

// Environment variables that keep pip quiet and non-interactive (no version nag, no cache prompts).
QStringList environment();

// Display form of a command: the program's bare name plus its arguments, home folder shortened to "~".
QString displayOf(const QString &program, const QStringList &arguments);

// `uv venv <dir>` / `python3 -m venv <dir>`.
Command createVenv(const QString &dir, bool useUv, const QString &basePython);
Command install(const QString &venvPython, const QStringList &specs, bool upgrade, bool useUv);
Command installRequirements(const QString &venvPython, const QString &file, bool useUv);
Command installProject(const QString &venvPython, const QString &projectDir, bool useUv); // editable install of a pyproject.toml / setup.py project
Command uninstall(const QString &venvPython, const QStringList &names, bool useUv);
Command list(const QString &venvPython, bool outdated, bool useUv); // JSON on stdout

// One row of `list` output.
struct Package {
    QString name;
    QString version;
    QString latest; // only for `outdated`
};
QList<Package> parseList(const QByteArray &json);

// Splits what the user typed into the install field ("requests flask django>=5") into requirements. Names, extras and
// version specifiers are accepted, and so are URL / VCS requirements; anything else (an option such as
// "--index-url=…", a typo) sets `error` and returns an empty list.
QStringList splitRequirements(const QString &text, QString *error);

// The PyPI package that provides an importable module: usually its own name, but not always ("yaml" is installed as
// "pyyaml", "PIL" as "pillow"). `module` may be dotted ("google.protobuf"); only the known top-level names are mapped.
QString packageForModule(const QString &module);

} // namespace PythonTools
