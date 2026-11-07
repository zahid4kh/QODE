#include "app/QodeFiles.h"

#include "jvm/JdkInstaller.h"
#include "lsp/LspInstaller.h"
#include "platform/AppPaths.h"
#include "platform/Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("QodeFiles", text);
}

bool exists(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() || fi.isSymLink();
}

} // namespace

namespace QodeFiles {

qint64 sizeOf(const QString &path)
{
    const QFileInfo top(path);
    if (!top.isDir() || top.isSymLink() || top.isJunction())
        return top.exists() ? top.size() : 0;
    qint64 total = 0;
    QStringList stack{path};
    while (!stack.isEmpty()) {
        const QDir dir(stack.takeLast());
        for (const QFileInfo &e : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (e.isSymLink() || e.isJunction())
                continue; // a link (Windows: the `current` junction of a language server) is not what it points at
            if (e.isDir())
                stack << e.absoluteFilePath();
            else
                total += e.size();
        }
    }
    return total;
}

QList<Item> created(bool sizes)
{
    QList<Item> out;
    const bool onUnix = Platform::osTag() != QLatin1String("win");
    const QString data = AppPaths::data();
    auto add = [&](Item item) {
        if (!exists(item.path))
            return;
        if (sizes)
            item.size = sizeOf(item.path);
        out << item;
    };

    add({QStringLiteral("config"), AppPaths::config(),
         onUnix ? tr("Settings, API keys (claude.key, gemini.key), MCP servers (mcp.json), per-project state (projects/), "
                   "Qt's file dialog state (QtProject.conf)")
              : tr("Settings, API keys (claude.key, gemini.key), MCP servers (mcp.json), per-project state (projects\\)"),
         true, true});
    add({QStringLiteral("data"), data,
         onUnix ? tr("Language servers QODE downloaded (lsp/), local history, scratch files, web preview storage")
              : tr("Language servers QODE downloaded (lsp\\), local history, scratch files, web preview storage, caches "
                   "(cache\\: server indexes, library sources)"),
         true, true});
    if (!Platform::isUnder(AppPaths::cache(), data)) // Windows keeps the cache inside the data folder
        add({QStringLiteral("cache"), AppPaths::cache(), tr("Caches: language server indexes, library sources, build helpers"), true, true});

    // Kotlin's command link (~/.local/bin/kotlin-lsp, Windows kotlin-lsp.cmd), only when it is QODE's own.
    const QString link = LspInstaller::linkPath();
    const QString target = Platform::commandLinkTarget(link);
    if (!target.isEmpty() && LspInstaller::isManaged(target))
        add({QStringLiteral("kotlin-link"), link, tr("Command link to the Kotlin language server QODE downloaded"), false, true});

    // JDKs QODE downloaded into the shared ~/.jdks (inside the data folder for portable QODE: already covered).
    int n = 0;
    for (const QString &home : JdkInstaller::downloaded()) {
        ++n;
        if (!Platform::isUnder(home, data))
            add({QStringLiteral("jdk-%1").arg(n), home,
                 tr("Eclipse Temurin JDK QODE downloaded (Gradle / Maven projects may still use it)"), true, false});
    }
    return out;
}

} // namespace QodeFiles
