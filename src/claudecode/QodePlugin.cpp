#include "QodePlugin.h"

#include "platform/AppPaths.h"
#include "platform/Platform.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

namespace claudecode::QodePlugin {

namespace {

// Skills that only work with the web preview.
bool needsPreview(const QString &skill)
{
    return skill == QLatin1String("check-ui") || skill == QLatin1String("expo-web");
}

bool hasPreview()
{
#ifdef QODE_HAS_WEBENGINE
    return true;
#else
    return false;
#endif
}

QByteArray resource(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Writes `data` to `path` unless it already holds exactly that.
bool put(const QString &path, const QByteArray &data)
{
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && f.readAll() == data)
            return true;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit();
}

// The shipped skills this build offers, by folder name.
QStringList skills()
{
    QStringList out;
    QDirIterator it(QStringLiteral(":/claude-code/skills"), QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString name = QFileInfo(it.next()).fileName();
        if (hasPreview() || !needsPreview(name))
            out << name;
    }
    out.sort();
    return out;
}

// system-prompt.md: lines marked [preview] only with the preview, @SKILLS@ = the skills offered.
QByteArray systemPrompt(const QStringList &names)
{
    QStringList lines;
    for (QString line : QString::fromUtf8(resource(QStringLiteral(":/claude-code/system-prompt.md"))).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("[preview]"))) {
            if (!hasPreview())
                continue;
            line.remove(0, 9);
        }
        lines << line;
    }
    return lines.join(QLatin1Char('\n')).replace(QLatin1String("@SKILLS@"), names.join(QLatin1String(", "))).toUtf8();
}

Files write()
{
    Files files;
    const QString root = AppPaths::data(QStringLiteral("claude-code"));
    const QString plugin = root + QStringLiteral("/plugin");
    const QStringList names = skills();

    bool ok = put(plugin + QStringLiteral("/.claude-plugin/plugin.json"),
                  resource(QStringLiteral(":/claude-code/plugin.json")).replace("@VERSION@", QODE_VERSION));
    QSet<QString> shipped;
    for (const QString &name : names) {
        shipped.insert(name);
        ok = put(QStringLiteral("%1/skills/%2/SKILL.md").arg(plugin, name),
                 resource(QStringLiteral(":/claude-code/skills/%1/SKILL.md").arg(name))) && ok;
    }
    // Skills an earlier QODE shipped (or the preview skills after switching to a build without the preview).
    const QDir skillDir(plugin + QStringLiteral("/skills"));
    for (const QString &name : skillDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!shipped.contains(name))
            Platform::removeTree(skillDir.filePath(name));
    if (ok)
        files.pluginDir = plugin;

    const QString prompt = root + QStringLiteral("/system-prompt.md");
    if (put(prompt, systemPrompt(names)))
        files.systemPrompt = prompt;
    return files;
}

} // namespace

Files install()
{
    return write();
}

} // namespace claudecode::QodePlugin
