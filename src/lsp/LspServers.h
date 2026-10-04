#pragma once

#include <QHash>
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
    // How QODE can set the server up itself: Download = Kotlin's archive (LspInstaller), Npm = `npm install` into a private
    // folder (NpmInstaller), shared by every server with the same managedId.
    enum class Installer { None, Download, Npm };
    Installer installer = Installer::None;
    bool installable = false; // installer != None
    QString managedId;        // folder under <data>/QODE/lsp/ (defaults to id)
    QString managedBinary;    // launcher inside that folder, e.g. "node_modules/.bin/vscode-html-language-server"
    bool needsNode = false;   // a Node.js script: `node` must be reachable by its "#!/usr/bin/env node" line
    // A companion runs next to the file's main server (ESLint, Tailwind CSS): it never becomes "the" server of a file,
    // but every document it applies to is opened in it too, and its diagnostics / completions / colours are merged in.
    bool companion = false;
    bool (*relevant)(const QString &projectRoot) = nullptr; // companions only start where this holds (e.g. eslint is a dependency)
    bool wantsConfiguration = false; // asks the client for settings through workspace/configuration (we answer in LspManager)
    QHash<QString, QString> languageIds; // extension -> LSP language id when it differs from `id`

    // LSP language id for a file ("c", "cpp", ...), or an empty string.
    QString languageId(const QString &path) const;
};

namespace LspServers {
// Where QODE keeps a server it downloaded itself ("<data>/QODE/lsp/<id>"), and that server's launcher.
QString managedDir(const QString &id);
QString managedExecutable(const LspServerSpec &spec); // empty when nothing is installed
// True when `path` resolves into the folder QODE installs this server's files into.
bool isManaged(const LspServerSpec &spec, const QString &path);
// Every server installed by the same managed install (the four web servers share one npm folder).
QList<const LspServerSpec *> sharingInstall(const LspServerSpec &spec);
// A Node.js executable: PATH first, then the usual version-manager folders (a desktop-launched QODE does not see the
// PATH nvm / fnm set up in a shell). Empty when there is none.
QString nodeExecutable();
QString npmExecutable(); // next to node, else PATH
const QList<LspServerSpec> &all();
const LspServerSpec *forFile(const QString &path); // the main server (never a companion)
// The companions that handle this file and are relevant to the project at `root`.
QList<const LspServerSpec *> companionsFor(const QString &path, const QString &root);
const LspServerSpec *byId(const QString &id);
// The configured path when it is executable, otherwise the first match on PATH, otherwise a copy QODE downloaded.
// Empty when not found.
QString locate(const LspServerSpec &spec, const QString &configuredPath);
// True when `root` (or its build/ folder) holds a compile_commands.json / compile_flags.txt.
bool hasCompileDatabase(const QString &root);
} // namespace LspServers
