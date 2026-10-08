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
    // folder (NpmInstaller), Jdtls = the Eclipse JDT language server's archive (JdtlsInstaller), Pip = a private virtual
    // environment with `pip install` / `uv pip install` (PipInstaller); shared by every server with the same managedId.
    enum class Installer { None, Download, Npm, Jdtls, Pip };
    Installer installer = Installer::None;
    bool installable = false; // installer != None
    QString managedId;        // folder under <data>/QODE/lsp/ (defaults to id)
    QString managedBinary;    // launcher inside that folder, e.g. "node_modules/.bin/vscode-html-language-server"
    bool needsNode = false;   // a Node.js script: `node` must be reachable by its "#!/usr/bin/env node" line
    // A companion runs next to the file's main server (ESLint, Tailwind CSS): it never becomes "the" server of a file,
    // but every document it applies to is opened in it too, and its diagnostics / completions / colours are merged in.
    bool companion = false;
    bool (*relevant)(const QString &projectRoot) = nullptr; // companions only start where this holds (e.g. eslint is a dependency)
    bool resolvesCompletions = false; // accepting an item asks completionItem/resolve first (jdtls adds its imports there)
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
// A Java runtime of at least `minMajor` (JAVA_HOME, PATH, then the usual JDK folders; a JAVA_HOME pointing at an older JDK
// does not hide a newer one on PATH). `executable` is empty when none qualifies; `major` is then the newest one found
// (0 = no Java at all).
struct JavaRuntime {
    QString executable;
    int major = 0;
};
JavaRuntime findJava(int minMajor);
// The command that starts the Eclipse JDT language server for `launcher` (its bin/jdtls, in QODE's download, another
// extracted archive or a distribution's package). An extracted archive is started with java directly (no Python
// wrapper); anything else is run as the wrapper with the Java runtime named. `workspaceDir` holds the server's
// configuration and data for one project. False (with `error`) when no usable Java is found.
bool jdtlsCommand(const QString &launcher, const QString &workspaceDir, QString *program, QStringList *arguments, QString *error);
// A Node.js executable: PATH first, then the usual version-manager folders (a desktop-launched QODE does not see the
// PATH nvm / fnm set up in a shell). Empty when there is none.
QString nodeExecutable();
QString npmExecutable(); // next to node, else PATH
// A Python 3 interpreter (python3 / python on PATH, then the usual folders); empty when there is none.
QString pythonExecutable();
// Astral's uv (PATH, then ~/.local/bin and ~/.cargo/bin); empty when it is not installed.
QString uvExecutable();
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
