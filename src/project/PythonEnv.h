#pragma once

#include <QString>

// Detects a Python virtual environment inside a project, whatever the folder is called.
namespace PythonEnv {

// Root-relative name of the project's venv folder ("" when none). A folder is a venv if it has a
// `pyvenv.cfg` (written by venv/virtualenv/uv/poetry), which is what makes custom names work.
QString findVenv(const QString &projectRoot);

// Every venv folder directly inside the project (conventional names first), root-relative.
QStringList findVenvs(const QString &projectRoot);

// The venv QODE works with: the one chosen for this project (SettingsManager::pythonVenv) when it still exists,
// otherwise findVenv. Terminal activation, the language server and the package manager all use this one.
QString activeVenv(const QString &projectRoot);

// Absolute path of the interpreter inside `venv` (root-relative name); empty when it has none.
QString venvPython(const QString &projectRoot, const QString &venv);

// "3.12.3" from the venv's pyvenv.cfg; empty when unknown.
QString venvVersion(const QString &projectRoot, const QString &venv);

// Shell command that activates the venv for `shellName` (bash, zsh, fish, ...), or "" when the
// project has no venv or the venv has no activation script for that shell.
QString activationCommand(const QString &projectRoot, const QString &shellName);

} // namespace PythonEnv
