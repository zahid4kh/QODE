#pragma once

#include <QString>
#include <QStringList>

// A language server QODE knows how to talk to. Servers are never bundled: the user installs them and
// QODE finds them on PATH (or at a path set in the LSP menu).
struct LspServerSpec {
    QString id;          // "clangd"; also the settings key
    QString displayName; // "clangd (C/C++)"
    QStringList executables; // names looked up on PATH, in order; "name-N" variants are tried too
    QStringList arguments;
    QStringList extensions; // lowercase file extensions this server handles
    bool fallbackFlags = false; // accepts initializationOptions.fallbackFlags (clangd)
    QString installHelp;    // shown when the executable is missing
    bool installable = false; // QODE can download and set the server up itself (LspInstaller)

    // LSP language id for a file ("c", "cpp", ...), or an empty string.
    QString languageId(const QString &path) const;
};

namespace LspServers {
// Where QODE keeps a server it downloaded itself ("<data>/QODE/lsp/<id>"), and that server's launcher.
QString managedDir(const QString &id);
QString managedExecutable(const QString &id); // empty when nothing is installed
const QList<LspServerSpec> &all();
const LspServerSpec *forFile(const QString &path);
const LspServerSpec *byId(const QString &id);
// The configured path when it is executable, otherwise the first match on PATH, otherwise a copy QODE downloaded.
// Empty when not found.
QString locate(const LspServerSpec &spec, const QString &configuredPath);
// True when `root` (or its build/ folder) holds a compile_commands.json / compile_flags.txt.
bool hasCompileDatabase(const QString &root);
} // namespace LspServers
