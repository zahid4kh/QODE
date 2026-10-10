#include "LspServers.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
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
    return Platform::dataDir() + QStringLiteral("/lsp/") + id;
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

// "java -version" prints `openjdk version "21.0.4" ...` (or "1.8.0_402" for Java 8): the major version, 0 when unknown.
static int javaMajor(const QString &javaPath)
{
    static QHash<QString, int> cache; // a launch asks for it again on every restart
    const QString key = QFileInfo(javaPath).canonicalFilePath();
    if (cache.contains(key))
        return cache.value(key);
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(javaPath, {QStringLiteral("-version")});
    int major = 0;
    if (p.waitForFinished(5000)) {
        const auto m = QRegularExpression(QStringLiteral("version \"(\\d+)(?:\\.(\\d+))?")).match(QString::fromLocal8Bit(p.readAll()));
        if (m.hasMatch())
            major = m.captured(1) == QLatin1String("1") && !m.captured(2).isEmpty() ? m.captured(2).toInt() : m.captured(1).toInt();
    } else {
        p.kill();
    }
    cache.insert(key, major);
    return major;
}

JavaRuntime findJava(int minMajor)
{
    QStringList candidates;
    const QString javaHome = qEnvironmentVariable("JAVA_HOME");
    if (!javaHome.isEmpty())
        candidates << javaHome + QStringLiteral("/bin/java");
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("java"));
    if (!onPath.isEmpty())
        candidates << onPath;
    const QString home = QDir::homePath();
    for (const QString &dir : {QStringLiteral("/usr/lib/jvm"), QStringLiteral("/usr/lib64/jvm"), QStringLiteral("/usr/java"), QStringLiteral("/opt/jdk"),
                               home + QStringLiteral("/.sdkman/candidates/java"), home + QStringLiteral("/.jdks"),
                               home + QStringLiteral("/.local/share/mise/installs/java"), home + QStringLiteral("/.asdf/installs/java")}) {
        QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) { return QString::compare(a, b, Qt::CaseInsensitive) > 0; });
        for (const QString &n : std::as_const(names))
            candidates << dir + QLatin1Char('/') + n + QStringLiteral("/bin/java");
    }
    JavaRuntime best;
    QSet<QString> seen;
    for (const QString &c : std::as_const(candidates)) {
        const QFileInfo fi(c);
        const QString real = fi.canonicalFilePath();
        if (real.isEmpty() || !fi.isExecutable() || seen.contains(real))
            continue;
        seen.insert(real);
        const int major = javaMajor(c);
        if (major >= minMajor)
            return {fi.absoluteFilePath(), major};
        best.major = qMax(best.major, major);
    }
    return best;
}

bool jdtlsCommand(const QString &launcher, const QString &workspaceDir, QString *program, QStringList *arguments, QString *error)
{
    const JavaRuntime java = findJava(21);
    if (java.executable.isEmpty()) {
        *error = java.major > 0 ? QObject::tr("The Java language server needs Java 21 or newer, but the newest Java found is %1. "
                                              "Install a JDK 21+ (see LSP > Java > How to Install), then choose Restart Server.")
                                      .arg(java.major)
                                : QObject::tr("The Java language server needs Java 21 or newer, and no Java was found. "
                                              "Install a JDK 21+ (see LSP > Java > How to Install), then choose Restart Server.");
        return false;
    }
    const QStringList tail = {QStringLiteral("-configuration"), workspaceDir + QStringLiteral("/config"), QStringLiteral("-data"),
                              workspaceDir + QStringLiteral("/data")};
    // An extracted archive: <base>/bin/jdtls, <base>/plugins/org.eclipse.equinox.launcher_*.jar, <base>/config_linux.
    QDir base(QFileInfo(QFileInfo(launcher).canonicalFilePath()).absolutePath());
    base.cdUp();
    const bool arm = QSysInfo::currentCpuArchitecture().startsWith(QLatin1String("arm64")) || QSysInfo::currentCpuArchitecture() == QLatin1String("aarch64");
    const QString config = base.filePath(arm && base.exists(QStringLiteral("config_linux_arm")) ? QStringLiteral("config_linux_arm") : QStringLiteral("config_linux"));
    const QStringList jars = QDir(base.filePath(QStringLiteral("plugins"))).entryList({QStringLiteral("org.eclipse.equinox.launcher_*.jar")}, QDir::Files);
    if (!jars.isEmpty() && QFileInfo(config).isDir()) {
        *program = java.executable;
        *arguments = {QStringLiteral("-Declipse.application=org.eclipse.jdt.ls.core.id1"),
                      QStringLiteral("-Dosgi.bundles.defaultStartLevel=4"),
                      QStringLiteral("-Declipse.product=org.eclipse.jdt.ls.core.product"),
                      QStringLiteral("-Dosgi.checkConfiguration=true"),
                      QStringLiteral("-Dosgi.sharedConfiguration.area=") + config,
                      QStringLiteral("-Dosgi.sharedConfiguration.area.readOnly=true"),
                      QStringLiteral("-Dosgi.configuration.cascaded=true"),
                      QStringLiteral("-Djava.import.generatesMetadataFilesAtProjectRoot=false"), // no .project / .classpath in the project
                      QStringLiteral("--add-modules=ALL-SYSTEM"),
                      QStringLiteral("--add-opens"), QStringLiteral("java.base/java.util=ALL-UNNAMED"),
                      QStringLiteral("--add-opens"), QStringLiteral("java.base/java.lang=ALL-UNNAMED")};
        if (java.major >= 24)
            *arguments = QStringList{QStringLiteral("-Djdk.xml.maxGeneralEntitySizeLimit=0"), QStringLiteral("-Djdk.xml.totalEntitySizeLimit=0")} + *arguments;
        *arguments += QStringList{QStringLiteral("-jar"), base.filePath(QStringLiteral("plugins/") + jars.first())};
        *arguments += tail;
        return true;
    }
    // A packaged "jdtls" (Python wrapper): it picks its own configuration; only tell it which Java and where to keep data.
    *program = launcher;
    *arguments = QStringList{QStringLiteral("--java-executable=") + java.executable,
                             QStringLiteral("--jvm-arg=-Djava.import.generatesMetadataFilesAtProjectRoot=false")} + tail;
    return true;
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

QString pythonExecutable()
{
    for (const char *name : {"python3", "python"}) {
        const QString onPath = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!onPath.isEmpty())
            return onPath;
    }
    for (const char *dir : {"/usr/bin", "/usr/local/bin", "/bin"}) {
        const QFileInfo fi(QString::fromLatin1(dir) + QStringLiteral("/python3"));
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return {};
}

QString uvExecutable()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("uv"));
    if (!onPath.isEmpty())
        return onPath;
    const QString home = QDir::homePath();
    for (const QString &dir : {home + QStringLiteral("/.local/bin"), home + QStringLiteral("/.cargo/bin"), QStringLiteral("/usr/local/bin")}) {
        const QFileInfo fi(dir + QStringLiteral("/uv"));
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return {};
}

namespace {
bool dependsOn(const QString &root, const QString &package)
{
    if (root.isEmpty())
        return false;
    if (QFileInfo(root + QStringLiteral("/node_modules/") + package).isDir())
        return true;
    QFile f(root + QStringLiteral("/package.json"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (const char *key : {"dependencies", "devDependencies"})
        if (o.value(QLatin1String(key)).toObject().contains(package))
            return true;
    return false;
}
} // namespace

// ESLint needs its own library in the project (the server loads it from there) and some configuration.
bool hasEslint(const QString &root)
{
    if (root.isEmpty() || !QFileInfo(root + QStringLiteral("/node_modules/eslint")).isDir())
        return false;
    const QStringList configs = QDir(root).entryList({QStringLiteral("eslint.config.*"), QStringLiteral(".eslintrc*")}, QDir::Files | QDir::Hidden);
    if (!configs.isEmpty())
        return true;
    QFile f(root + QStringLiteral("/package.json"));
    return f.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(f.readAll()).object().contains(QStringLiteral("eslintConfig"));
}

bool hasTailwind(const QString &root)
{
    if (dependsOn(root, QStringLiteral("tailwindcss")))
        return true;
    return !root.isEmpty() && !QDir(root).entryList({QStringLiteral("tailwind.config.*")}, QDir::Files).isEmpty();
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

        LspServerSpec java;
        java.id = QStringLiteral("java");
        java.displayName = QStringLiteral("Java language server (Eclipse JDT)");
        java.executables = {QStringLiteral("jdtls")};
        java.extensions = {QStringLiteral("java")};
        java.installer = LspServerSpec::Installer::Jdtls;
        java.installable = true;
        java.managedId = QStringLiteral("jdtls");
        java.managedBinary = QStringLiteral("current/bin/jdtls");
        java.resolvesCompletions = true;
        java.installHelp = QStringLiteral(
            "The Java language server is the Eclipse JDT Language Server (jdtls), the one VS Code, Neovim and Eclipse use. It "
            "needs a Java runtime of version 21 or newer to run (any JDK; your projects can still target older Java versions).\n\n"
            "Easiest: LSP > Java > Download and Set Up. QODE downloads the milestone build from download.eclipse.org "
            "(about 50 MB) and unpacks it to ~/.local/share/QODE/lsp/jdtls. Nothing is installed system-wide.\n\n"
            "Java 21 or newer: sudo apt install openjdk-21-jdk (Debian / Ubuntu), sudo dnf install java-21-openjdk-devel (Fedora), "
            "sudo pacman -S jdk21-openjdk (Arch), or https://adoptium.net\n\n"
            "By hand: download and extract a build from https://download.eclipse.org/jdtls/milestones/ and point QODE at its "
            "bin/jdtls with LSP > Set Server Path (a \"jdtls\" package from your distribution works too).\n\n"
            "Projects should be Maven or Gradle projects (pom.xml or build.gradle(.kts) in the project folder) so the server "
            "finds their dependencies; the first import can take a minute while Maven / Gradle download them.");
        l.append(java);

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

        // Companions: installed by the same npm install, started only for projects that use them.
        LspServerSpec eslint = webServer(QStringLiteral("eslint"), QStringLiteral("ESLint"), QStringLiteral("vscode-eslint-language-server"),
                                         {QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"), QStringLiteral("cjs"),
                                          QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("mts"), QStringLiteral("cts")});
        eslint.languageIds = ts.languageIds;
        eslint.companion = true;
        eslint.wantsConfiguration = true;
        eslint.relevant = [](const QString &root) { return hasEslint(root); };
        eslint.installHelp = QStringLiteral(
            "ESLint reports lint problems as you type. It needs ESLint itself in the project (npm install -D eslint, with an "
            "eslint.config.js or .eslintrc file) and starts only for such projects. The language server comes with "
            "LSP > ESLint > Download and Set Up (the same npm install as the other web servers).");
        l.append(eslint);

        LspServerSpec tailwind = webServer(QStringLiteral("tailwindcss"), QStringLiteral("Tailwind CSS"),
                                           QStringLiteral("tailwindcss-language-server"),
                                           {QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("css"), QStringLiteral("jsx"),
                                            QStringLiteral("tsx"), QStringLiteral("js"), QStringLiteral("ts")});
        tailwind.languageIds = ts.languageIds;
        tailwind.languageIds.insert(QStringLiteral("html"), QStringLiteral("html"));
        tailwind.languageIds.insert(QStringLiteral("htm"), QStringLiteral("html"));
        tailwind.languageIds.insert(QStringLiteral("css"), QStringLiteral("css"));
        tailwind.companion = true;
        tailwind.wantsConfiguration = true;
        tailwind.relevant = [](const QString &root) { return hasTailwind(root); };
        tailwind.installHelp = QStringLiteral(
            "Tailwind CSS IntelliSense completes class names, shows what a class does on hover, previews colours and warns "
            "about conflicting classes. It starts only for projects that depend on tailwindcss. Install it with "
            "LSP > Tailwind CSS > Download and Set Up (the official @tailwindcss/language-server, installed with the other web "
            "servers by one npm install; Node.js is required).");
        l.append(tailwind);
        // Python: one private virtual environment (PipInstaller) provides basedpyright and the ruff companion.
        LspServerSpec python;
        python.id = QStringLiteral("python");
        python.displayName = QStringLiteral("Python (basedpyright)");
        python.executables = {QStringLiteral("basedpyright-langserver")};
        python.arguments = {QStringLiteral("--stdio")};
        python.extensions = {QStringLiteral("py"), QStringLiteral("pyi"), QStringLiteral("pyw")};
        python.installer = LspServerSpec::Installer::Pip;
        python.installable = true;
        python.managedId = QStringLiteral("python");
        python.managedBinary = QStringLiteral("bin/basedpyright-langserver");
        python.wantsConfiguration = true; // pythonPath (the project's venv) and the analysis settings
        python.installHelp = QStringLiteral(
            "The Python language server is basedpyright, a fork of Microsoft's Pyright: syntax and type errors, undefined names, "
            "hover, go to definition, completion, rename and find references. It needs Python 3 but no Node.js (it brings its "
            "own).\n\n"
            "Easiest: LSP > Python > Download and Set Up. QODE creates a private virtual environment in "
            "~/.local/share/QODE/lsp/python and installs basedpyright and ruff into it (with uv when you have it, otherwise "
            "pip). Nothing is installed globally.\n\n"
            "By hand: pipx install basedpyright (or pip install basedpyright in any environment), then make sure "
            "\"basedpyright-langserver\" is on your PATH, or point QODE at it with LSP > Set Server Path.\n\n"
            "Python 3: sudo apt install python3 python3-venv (Debian / Ubuntu), sudo dnf install python3 (Fedora), "
            "sudo pacman -S python (Arch).\n\n"
            "Open a project that has a virtual environment (a folder with pyvenv.cfg, such as .venv) and the server resolves "
            "imports from its installed packages.");
        l.append(python);

        LspServerSpec ruff;
        ruff.id = QStringLiteral("ruff");
        ruff.displayName = QStringLiteral("Ruff (Python linter)");
        ruff.executables = {QStringLiteral("ruff")};
        ruff.arguments = {QStringLiteral("server")};
        ruff.extensions = {QStringLiteral("py"), QStringLiteral("pyi"), QStringLiteral("pyw")};
        ruff.languageIds = {{QStringLiteral("py"), QStringLiteral("python")}, {QStringLiteral("pyi"), QStringLiteral("python")},
                            {QStringLiteral("pyw"), QStringLiteral("python")}};
        ruff.installer = LspServerSpec::Installer::Pip;
        ruff.installable = true;
        ruff.managedId = QStringLiteral("python");
        ruff.managedBinary = QStringLiteral("bin/ruff");
        ruff.companion = true;
        ruff.installHelp = QStringLiteral(
            "Ruff is an extremely fast Python linter and formatter. Next to basedpyright it reports lint problems as you type "
            "(unused imports, style, common bugs), offers quick fixes with Alt+Enter, and formats the file on save. It comes "
            "with LSP > Ruff > Download and Set Up (the same install as the Python server).\n\n"
            "By hand: pipx install ruff (or pip install ruff), and make sure \"ruff\" is on your PATH.");
        l.append(ruff);
        return l;
    }();
    return specs;
}

const LspServerSpec *forFile(const QString &path)
{
    if (path.isEmpty())
        return nullptr;
    for (const LspServerSpec &s : all())
        if (!s.companion && !s.languageId(path).isEmpty())
            return &s;
    return nullptr;
}

QList<const LspServerSpec *> companionsFor(const QString &path, const QString &root)
{
    QList<const LspServerSpec *> out;
    for (const LspServerSpec &s : all())
        if (s.companion && !s.languageId(path).isEmpty() && (!s.relevant || s.relevant(root)))
            out << &s;
    return out;
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
