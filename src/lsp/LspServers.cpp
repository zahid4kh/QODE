#include "LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

QString LspServerSpec::languageId(const QString &path) const
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (!extensions.contains(ext))
        return {};
    if (id == QLatin1String("clangd"))
        return ext == QLatin1String("c") ? QStringLiteral("c") : QStringLiteral("cpp");
    return id;
}

namespace LspServers {

QString managedDir(const QString &id)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/QODE/lsp/") + id;
}

QString managedExecutable(const QString &id)
{
    const QFileInfo fi(managedDir(id) + QStringLiteral("/current/bin/intellij-server"));
    return fi.isFile() && fi.isExecutable() ? fi.absoluteFilePath() : QString();
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
        kotlin.installable = true;
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
    return spec.installable ? managedExecutable(spec.id) : QString();
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
