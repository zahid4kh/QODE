#pragma once

#include <QString>
#include <QStringList>

// The command lines QODE types into a terminal (Run / Debug, tests, Gradle and Maven tasks, Spring Boot, Ktor, Expo,
// run configurations) are written in a small part of sh: words, 'single quoted' ('\'' for a quote) or "double quoted"
// (no $), VAR=value before a command, &&, ||, ;, | and one ( … ) around the whole line (a subshell with a cd).
// translate() rewrites such a line for the shell of the pane that runs it:
// - sh, bash, zsh, fish (Unix): unchanged, as before.
// - bash.exe / sh.exe (Git Bash): unchanged, except .\gradlew -> ./gradlew.
// - wsl.exe (a Linux shell on Windows): .\gradlew -> ./gradlew and C:/… -> /mnt/c/… (WSL's default mounts).
// - PowerShell (5.1 and 7): quoting, `a; if ($?) { b }` for &&, $env: for VAR=, Push-Location for the subshell. What a
//   subshell or VAR= changed is restored in a finally block, which leaves $? true: the line then puts its real result
//   in $__qodeOk, which the shell integration reads (ShellIntegration.cpp).
// - cmd: "quoting", & for ;, a program path with '/' gets '\' (cmd reads ./x as the command '.'), and a child cmd for
//   the subshell and VAR= (cmd cannot restore them itself).
// - PowerShell and cmd: `mkdir -p a b` becomes New-Item -Force / `(if not exist "a\" md "a")`.
// A line outside that subset ($VAR, `…`, globs, redirections) is passed on unchanged.
namespace ShellSyntax {

enum class Dialect { Posix, GitBash, Wsl, PowerShell, Cmd };

// From the shell's program path or name: "pwsh.exe", "powershell", "cmd.exe", "C:/…/Git/bin/bash.exe", "/bin/bash" …
Dialect dialectOf(const QString &shell);

QString translate(const QString &line, const QString &shell);

// One word quoted for `dialect`: PowerShell '…' (always: a string in an expression too), cmd "…" when it needs it,
// sh '…'.
QString quote(Dialect dialect, const QString &word);

// Commands run one after the other: "; " between them, " & " for cmd.
QString sequence(Dialect dialect, const QStringList &commands);

} // namespace ShellSyntax
