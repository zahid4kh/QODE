#include "LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>
#include <algorithm>

QString LspServerSpec::languageId(const QString &path) const
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (!extensions.contains(ext))
        return {};
    if (id == QLatin1String("clangd"))
        return ext == QLatin1String("c") ? QStringLiteral("c") : QStringLiteral("cpp");
    // tsconfig.json, .eslintrc.json ... allow comments: a plain "json" id would flag them as errors.
    if (id == QLatin1String("json")) {
        const QString name = QFileInfo(path).fileName().toLower();
        if (ext == QLatin1String("jsonc") || name.startsWith(QLatin1String("tsconfig")) || name.startsWith(QLatin1String("jsconfig")) ||
            name.startsWith(QLatin1String(".eslintrc")) || name == QLatin1String("devcontainer.json"))
            return QStringLiteral("jsonc");
    }
    return languageIds.value(ext, id);
}

namespace LspServers {

QString managedDir(const QString &id)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/QODE/lsp/") + id;
}

QString managedExecutable(const LspServerSpec &spec)
{
    if (!spec.installable)
        return {};
    const QFileInfo fi(managedDir(spec.managedId) + QLatin1Char('/') + spec.managedBinary);
    return fi.isFile() && fi.isExecutable() ? fi.absoluteFilePath() : QString();
}

bool isManaged(const LspServerSpec &spec, const QString &path)
{
    if (!spec.installable)
        return false;
    const QString real = QFileInfo(path).canonicalFilePath();
    const QString root = QFileInfo(managedDir(spec.managedId)).canonicalFilePath();
    return !real.isEmpty() && !root.isEmpty() && real.startsWith(root + QLatin1Char('/'));
}

QList<const LspServerSpec *> sharingInstall(const LspServerSpec &spec)
{
    QList<const LspServerSpec *> out;
    for (const LspServerSpec &s : all())
        if (s.installable && s.managedId == spec.managedId)
            out << &s;
    return out;
}

QString nodeExecutable()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (!onPath.isEmpty())
        return onPath;
    const QString home = QDir::homePath();
    // Newest first inside version-manager folders (v22.1.0 before v18.0.0).
    auto newest = [](const QString &dir, const QString &sub) {
        QStringList versions = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
            return QVersionNumber::fromString(a.mid(a.startsWith(QLatin1Char('v')) ? 1 : 0)) >
                   QVersionNumber::fromString(b.mid(b.startsWith(QLatin1Char('v')) ? 1 : 0));
        });
        QStringList out;
        for (const QString &v : std::as_const(versions))
            out << dir + QLatin1Char('/') + v + sub;
        return out;
    };
    QStringList dirs;
    dirs << newest(home + QStringLiteral("/.nvm/versions/node"), QStringLiteral("/bin"));
    dirs << newest(home + QStringLiteral("/.local/share/fnm/node-versions"), QStringLiteral("/installation/bin"));
    dirs << home + QStringLiteral("/.volta/bin") << home + QStringLiteral("/.local/share/mise/shims")
         << home + QStringLiteral("/.asdf/shims") << home + QStringLiteral("/.local/bin") << QStringLiteral("/usr/local/bin")
         << QStringLiteral("/usr/bin") << QStringLiteral("/snap/bin");
    for (const QString &dir : std::as_const(dirs)) {
        const QFileInfo fi(dir + QStringLiteral("/node"));
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return {};
}

QString npmExecutable()
{
    const QString node = nodeExecutable();
    if (!node.isEmpty()) {
        const QFileInfo fi(QFileInfo(node).absolutePath() + QStringLiteral("/npm"));
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("npm"));
}

const QList<LspServerSpec> &all()
{
    static const QList<LspServerSpec> specs = [] {
        QList<LspServerSpec> l;
        LspServerSpec clangd;
        clangd.id = QStringLiteral("clangd");
        clangd.displayName = QStringLiteral("clangd (C/C++)");
        clangd.executables = {QStringLiteral("clangd")};
        clangd.arguments = {QStringLiteral("--background-index"), QStringLiteral("--log=error"),
                            QStringLiteral("--header-insertion=never")};
        clangd.extensions = {QStringLiteral("c"),   QStringLiteral("cc"),  QStringLiteral("cpp"), QStringLiteral("cxx"),
                             QStringLiteral("c++"), QStringLiteral("h"),   QStringLiteral("hh"),  QStringLiteral("hpp"),
                             QStringLiteral("hxx"), QStringLiteral("h++"), QStringLiteral("ipp"), QStringLiteral("tpp"),
                             QStringLiteral("inl")};
        clangd.fallbackFlags = true;
        clangd.managedId = clangd.id;
        clangd.installHelp = QStringLiteral(
            "clangd is the C/C++ language server from the LLVM project. Install it with your package manager:\n\n"
            "  Debian / Ubuntu:   sudo apt install clangd\n"
            "  Fedora:            sudo dnf install clang-tools-extra\n"
            "  Arch / Manjaro:    sudo pacman -S clang\n"
            "  openSUSE:          sudo zypper install clang-tools\n\n"
            "Make sure it is on your PATH (run \"clangd --version\" in a terminal), or point QODE at it with "
            "LSP > Set Server Path. Releases are also published at https://github.com/clangd/clangd/releases");
        l.append(clangd);

        LspServerSpec kotlin;
        kotlin.id = QStringLiteral("kotlin");
        kotlin.displayName = QStringLiteral("Kotlin language server");
        kotlin.executables = {QStringLiteral("kotlin-lsp"), QStringLiteral("kotlin-lsp.sh")};
        // {cache} becomes a per-project folder under ~/.cache/QODE (the server otherwise indexes into a fresh /tmp folder).
        kotlin.arguments = {QStringLiteral("--stdio"), QStringLiteral("--system-path={cache}")};
        kotlin.extensions = {QStringLiteral("kt"), QStringLiteral("kts")};
        kotlin.installer = LspServerSpec::Installer::Download;
        kotlin.installable = true;
        kotlin.managedId = QStringLiteral("kotlin");
        kotlin.managedBinary = QStringLiteral("current/bin/intellij-server");
        kotlin.installHelp = QStringLiteral(
            "The Kotlin language server is JetBrains' official server (Alpha). It ships its own Java runtime, so you do "
            "not need to install Java for it.\n\n"
            "Easiest: LSP > Kotlin > Download and Set Up. QODE downloads the standalone Linux archive from "
            "download.jetbrains.com (about 370 MB), unpacks it to ~/.local/share/QODE/lsp/kotlin and links it as "
            "~/.local/bin/kotlin-lsp.\n\n"
            "By hand: download the standalone archive (kotlin-server-<version>.tar.gz) from "
            "https://github.com/Kotlin/kotlin-lsp/releases, unpack it, and put bin/intellij-server on your PATH as "
            "\"kotlin-lsp\" (for example ln -s <folder>/bin/intellij-server ~/.local/bin/kotlin-lsp), or point QODE at it "
            "with LSP > Set Server Path. Do not use the .vsix file: that is the VS Code extension.\n\n"
            "Projects should be Gradle or Maven projects (a build.gradle(.kts) or pom.xml in the project folder) so the "
            "server can find your dependencies.");
        l.append(kotlin);

        // The web servers: one private `npm install` (NpmInstaller) provides all four, so they share a managedId.
        auto webServer = [](const QString &id, const QString &name, const QString &exe, const QStringList &exts) {
            LspServerSpec s;
            s.id = id;
            s.displayName = name;
            s.executables = {exe};
            s.arguments = {QStringLiteral("--stdio")};
            s.extensions = exts;
            s.installer = LspServerSpec::Installer::Npm;
            s.installable = true;
            s.managedId = QStringLiteral("web");
            s.managedBinary = QStringLiteral("node_modules/.bin/") + exe;
            s.needsNode = true;
            s.installHelp = QStringLiteral(
                "%1 is one of the web language servers. They are Node.js programs, so Node.js (with npm) must be installed.\n\n"
                "Easiest: LSP > %2 > Download and Set Up. QODE runs \"npm install\" into ~/.local/share/QODE/lsp/web, which "
                "sets up the TypeScript / JavaScript, HTML, CSS and JSON servers together. Nothing is installed globally.\n\n"
                "By hand: npm install -g typescript typescript-language-server vscode-langservers-extracted\n"
                "Then make sure \"%3\" is on your PATH, or point QODE at it with LSP > Set Server Path.\n\n"
                "Node.js: sudo apt install nodejs npm (Debian / Ubuntu), sudo dnf install nodejs (Fedora), "
                "sudo pacman -S nodejs npm (Arch), or https://nodejs.org")
                               .arg(name, name, exe);
            return s;
        };
        LspServerSpec ts = webServer(QStringLiteral("typescript"), QStringLiteral("TypeScript / JavaScript"),
                                     QStringLiteral("typescript-language-server"),
                                     {QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("mts"), QStringLiteral("cts"),
                                      QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"), QStringLiteral("cjs")});
        ts.languageIds = {{QStringLiteral("ts"), QStringLiteral("typescript")},      {QStringLiteral("mts"), QStringLiteral("typescript")},
                          {QStringLiteral("cts"), QStringLiteral("typescript")},     {QStringLiteral("tsx"), QStringLiteral("typescriptreact")},
                          {QStringLiteral("js"), QStringLiteral("javascript")},      {QStringLiteral("mjs"), QStringLiteral("javascript")},
                          {QStringLiteral("cjs"), QStringLiteral("javascript")},     {QStringLiteral("jsx"), QStringLiteral("javascriptreact")}};
        l.append(ts);

        LspServerSpec html = webServer(QStringLiteral("html"), QStringLiteral("HTML"), QStringLiteral("vscode-html-language-server"),
                                       {QStringLiteral("html"), QStringLiteral("htm")});
        l.append(html);

        LspServerSpec css = webServer(QStringLiteral("css"), QStringLiteral("CSS / SCSS / Less"), QStringLiteral("vscode-css-language-server"),
                                      {QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("less")});
        css.languageIds = {{QStringLiteral("scss"), QStringLiteral("scss")}, {QStringLiteral("less"), QStringLiteral("less")}};
        l.append(css);

        LspServerSpec json = webServer(QStringLiteral("json"), QStringLiteral("JSON"), QStringLiteral("vscode-json-language-server"),
                                       {QStringLiteral("json"), QStringLiteral("jsonc")});
        l.append(json);
        return l;
    }();
    return specs;
}

const LspServerSpec *forFile(const QString &path)
{
    if (path.isEmpty())
        return nullptr;
    for (const LspServerSpec &s : all())
        if (!s.languageId(path).isEmpty())
            return &s;
    return nullptr;
}

const LspServerSpec *byId(const QString &id)
{
    for (const LspServerSpec &s : all())
        if (s.id == id)
            return &s;
    return nullptr;
}

QString locate(const LspServerSpec &spec, const QString &configuredPath)
{
    if (!configuredPath.isEmpty()) {
        const QFileInfo fi(configuredPath);
        return fi.isFile() && fi.isExecutable() ? fi.absoluteFilePath() : QString();
    }
    for (const QString &name : spec.executables) {
        const QString exact = QStandardPaths::findExecutable(name);
        if (!exact.isEmpty())
            return exact;
        // Distributions often ship only versioned binaries ("clangd-18"): take the newest.
        static const QChar sep = QDir::listSeparator();
        QString best;
        int bestVersion = -1;
        const QRegularExpression re(QStringLiteral("^%1-(\\d+)$").arg(QRegularExpression::escape(name)));
        for (const QString &dir : qEnvironmentVariable("PATH").split(sep, Qt::SkipEmptyParts)) {
            const QStringList names = QDir(dir).entryList(QDir::Files | QDir::Executable);
            for (const QString &n : names) {
                const auto m = re.match(n);
                if (m.hasMatch() && m.captured(1).toInt() > bestVersion) {
                    bestVersion = m.captured(1).toInt();
                    best = dir + QLatin1Char('/') + n;
                }
            }
        }
        if (!best.isEmpty())
            return best;
    }
    return managedExecutable(spec);
}

bool hasCompileDatabase(const QString &root)
{
    if (root.isEmpty())
        return false;
    const QDir d(root);
    return d.exists(QStringLiteral("compile_commands.json")) || d.exists(QStringLiteral("compile_flags.txt")) ||
           d.exists(QStringLiteral("build/compile_commands.json"));
}

} // namespace LspServers
