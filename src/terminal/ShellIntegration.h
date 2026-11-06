#pragma once

#include <QProcessEnvironment>
#include <QStringList>

// Hooks for bash, zsh, fish and PowerShell that mark where each command starts (OSC 133;C, with the command line as
// OSC 633;E) and how it ended (OSC 133;D;<exit code>), so the terminal knows which output belongs to a failed command
// ("Fix with QODE"). The user's own startup files still run first; the scripts live in ~/.cache/QODE-AGENTIC/shell
// (PowerShell's goes on its command line). Where the OS cannot tell QODE the shell's folder (Windows: PowerShell, Git
// Bash), the prompt also sends it as OSC 633;P;Cwd=<folder>.
namespace ShellIntegration {

// Adds what `shell` (a path) needs to load the hooks to `args` and `env`; false (nothing changed) for other shells or
// when the scripts cannot be written.
bool apply(const QString &shell, QStringList *args, QProcessEnvironment *env);

} // namespace ShellIntegration
