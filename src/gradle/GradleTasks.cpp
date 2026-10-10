#include "GradleTasks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace GradleTasks {

namespace {
bool hasBuildFile(const QDir &d)
{
    for (const QString &f : {QStringLiteral("settings.gradle"), QStringLiteral("settings.gradle.kts"), QStringLiteral("build.gradle"),
                             QStringLiteral("build.gradle.kts")})
        if (d.exists(f))
            return true;
    return false;
}
} // namespace

QString buildDir(const QString &root)
{
    if (root.isEmpty())
        return {};
    const QDir top(root);
    if (hasBuildFile(top))
        return top.absolutePath();
    const QStringList subs = top.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &s : subs) {
        if (s.startsWith(QLatin1Char('.')) || s == QLatin1String("node_modules") || s == QLatin1String("build"))
            continue;
        const QDir d(top.filePath(s));
        if (hasBuildFile(d))
            return d.absolutePath();
    }
    return {};
}

QString runner(const QString &dir)
{
    return QFileInfo(dir + QStringLiteral("/gradlew")).isExecutable() ? QStringLiteral("./gradlew") : QStringLiteral("gradle");
}

QList<Group> parse(const QString &output)
{
    static const QRegularExpression entry(QStringLiteral("^([A-Za-z0-9_:.\\-]+)(?: - (.*))?$"));
    static const QRegularExpression rule(QStringLiteral("^-{3,}$"));
    QList<Group> groups;
    const QStringList lines = output.split(QLatin1Char('\n'));
    bool inGroup = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = QString(lines[i]).remove(QLatin1Char('\r')).trimmed();
        // A heading is a line followed by a row of dashes.
        if (i + 1 < lines.size() && rule.match(lines[i + 1].trimmed()).hasMatch() && !line.isEmpty() && !rule.match(line).hasMatch()) {
            if (line.startsWith(QLatin1String("Tasks runnable")) || line.startsWith(QLatin1String("All tasks runnable")) ||
                line == QLatin1String("Rules")) {
                inGroup = false;
                ++i;
                continue;
            }
            QString title = line;
            if (title.endsWith(QLatin1String(" tasks")))
                title.chop(6);
            groups.append({title, {}});
            inGroup = true;
            ++i;
            continue;
        }
        if (!inGroup || line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("To see ")) || line.startsWith(QLatin1String("BUILD ")) || line.startsWith(QLatin1String("Usage:")) ||
            line.startsWith(QLatin1String("gradlew ")))
            continue;
        const QRegularExpressionMatch m = entry.match(line);
        if (m.hasMatch() && !m.captured(1).endsWith(QLatin1Char(':'))) {
            groups.last().tasks.append({m.captured(1), m.captured(2)});
        } else if (!groups.last().tasks.isEmpty() && !groups.last().tasks.last().description.isEmpty()) {
            groups.last().tasks.last().description += QLatin1Char(' ') + line; // Gradle wraps long descriptions
        }
    }
    QList<Group> kept;
    for (const Group &g : groups)
        if (!g.tasks.isEmpty())
            kept.append(g);
    return kept;
}

} // namespace GradleTasks
