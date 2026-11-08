#include "app/QodeFiles.h"

#include "jvm/JdkInstaller.h"
#include "lsp/LspInstaller.h"
#include "platform/AppPaths.h"
#include "platform/Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cstring>

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
         onUnix ? tr("Settings, API keys (claude.key, gemini.key), MCP servers (mcp.json), per-project state (projects/), game scores (games/), "
                   "Qt's file dialog state (QtProject.conf)")
              : tr("Settings, API keys (claude.key, gemini.key), MCP servers (mcp.json), per-project state (projects\\), "
                   "game scores (games\\)"),
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

bool remove(const Item &item, QString *error)
{
    const QFileInfo fi(item.path);
    if (!fi.exists() && !fi.isSymLink())
        return true;
    const bool ok = fi.isDir() && !fi.isSymLink() && !fi.isJunction() ? Platform::removeTree(item.path) : QFile::remove(item.path);
    if (ok && !exists(item.path))
        return true;
    if (error)
        *error = tr("could not delete everything in it (a file may be in use)");
    return false;
}

bool runIfHelper(int argc, char *argv[])
{
    if (argc < 4 || std::strcmp(argv[1], "--qode-files") != 0)
        return false;
    // An application object for QStandardPaths, and the arguments in Unicode (not argv's ANSI code page on Windows).
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("QODE-AGENTIC"));
    QCoreApplication::setApplicationName(QStringLiteral("QODE"));
    const QStringList args = QCoreApplication::arguments();
    const QString mode = args.value(2);
    QStringList lines;
    if (mode == QLatin1String("list")) {
        for (const Item &i : created(true))
            lines << QStringList{i.id, QString::number(i.removeByDefault ? 1 : 0), QString::number(i.size), QDir::toNativeSeparators(i.path), i.what}
                         .join(QLatin1Char('\t'));
    } else if (mode == QLatin1String("remove")) {
        const QList<Item> items = created(false);
        for (const QString &id : args.mid(4)) {
            const auto it = std::find_if(items.cbegin(), items.cend(), [&id](const Item &i) { return i.id == id; });
            if (it == items.cend()) {
                lines << QStringLiteral("missing\t%1").arg(id);
                continue;
            }
            const qint64 size = sizeOf(it->path);
            QString error;
            lines << (remove(*it, &error) ? QStringLiteral("removed\t%1\t%2\t%3").arg(id, QDir::toNativeSeparators(it->path)).arg(size)
                                          : QStringLiteral("failed\t%1\t%2\t%3").arg(id, QDir::toNativeSeparators(it->path), error));
        }
    } else {
        return true;
    }
    QSaveFile report(args.value(3));
    if (report.open(QIODevice::WriteOnly)) {
        report.write("\xEF\xBB\xBF"); // UTF-8 BOM: Inno Setup's LoadStringsFromFile reads the file as UTF-8 then
        report.write(lines.join(QStringLiteral("\r\n")).toUtf8());
        report.write("\r\n");
        report.commit();
    }
    return true;
}

} // namespace QodeFiles
