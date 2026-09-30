#pragma once

#include <QString>

// Detects a Python virtual environment inside a project, whatever the folder is called.
namespace PythonEnv {

// Root-relative name of the project's venv folder ("" when none). A folder is a venv if it has a
// `pyvenv.cfg` (written by venv/virtualenv/uv/poetry), which is what makes custom names work.
QString findVenv(const QString &projectRoot);

// Shell command that activates the venv for `shellName` (bash, zsh, fish, ...), or "" when the
// project has no venv or the venv has no activation script for that shell.
QString activationCommand(const QString &projectRoot, const QString &shellName);

} // namespace PythonEnv
