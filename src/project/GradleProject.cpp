#include "project/GradleProject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace {

QString buildFile(const QString &dir)
{
    for (const char *name : {"build.gradle.kts", "build.gradle"}) {
        const QString p = dir + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

QString readHead(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(256 * 1024));
}

struct Kind { bool runnable = false, compose = false; };

Kind classify(const QString &text)
{
    Kind k;
    static const QRegularExpression composeApp(QStringLiteral(R"(compose\.desktop(\.application)?\s*\{|compose\.desktop\.application|\bdesktop\s*\.\s*application\b)"));
    static const QRegularExpression composePlugin(QStringLiteral(R"(org\.jetbrains\.compose|jetbrainsCompose|composeMultiplatform|compose\.multiplatform)"));
    static const QRegularExpression appPlugin(QStringLiteral(R"((id\s*\(\s*["']application["']\s*\)|^\s*application\s*$|apply\s+plugin:\s*["']application["']|\bapplication\s*\{[^}]*mainClass))"),
                                              QRegularExpression::MultilineOption);
    if (composeApp.match(text).hasMatch()) {
        k.runnable = k.compose = true;
    } else if (composePlugin.match(text).hasMatch() && text.contains(QStringLiteral("application"))) {
        k.runnable = k.compose = true;
    } else if (appPlugin.match(text).hasMatch()) {
        k.runnable = true;
    }
    return k;
}

} // namespace

bool GradleProject::isGradleFileKey(const QString &key)
{
    static const QSet<QString> keys = {QStringLiteral("kt"), QStringLiteral("kts"), QStringLiteral("java"), QStringLiteral("gradle"),
                                       QStringLiteral("properties"), QStringLiteral("toml"), QStringLiteral("xml")};
    return keys.contains(key);
}

GradleProject GradleProject::detect(const QString &root)
{
    GradleProject g;
    if (root.isEmpty())
        return g;
    // The wrapper lives in the root even when the application is a module.
    const bool wrapper = QFileInfo::exists(root + QStringLiteral("/gradlew"));
    if (!wrapper && buildFile(root).isEmpty() && !QFileInfo::exists(root + QStringLiteral("/settings.gradle.kts"))
        && !QFileInfo::exists(root + QStringLiteral("/settings.gradle")))
        return g;
    const QString tool = wrapper ? (QFileInfo(root + QStringLiteral("/gradlew")).isExecutable() ? QStringLiteral("./gradlew")
                                                                                              : QStringLiteral("sh gradlew"))
                                 : QStringLiteral("gradle");

    Kind best;
    QString module;
    if (const QString f = buildFile(root); !f.isEmpty())
        best = classify(readHead(f));
    if (!best.runnable) {
        // Multi-module builds (e.g. the Compose Multiplatform template: composeApp/, desktopApp/).
        const QStringList dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &d : dirs) {
            if (d.startsWith(QLatin1Char('.')) || d == QLatin1String("build") || d == QLatin1String("buildSrc") || d == QLatin1String("gradle"))
                continue;
            const QString f = buildFile(root + QLatin1Char('/') + d);
            if (f.isEmpty())
                continue;
            const Kind k = classify(readHead(f));
            if (k.runnable && (!best.runnable || (k.compose && !best.compose))) {
                best = k;
                module = d;
            }
        }
    }
    if (!best.runnable)
        return g;
    g.m_compose = best.compose;
    g.m_module = module;
    g.m_command = tool + QLatin1Char(' ') + (module.isEmpty() ? QString() : QLatin1Char(':') + module + QLatin1Char(':')) + QStringLiteral("run");
    return g;
}
