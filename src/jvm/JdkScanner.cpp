#include "jvm/JdkScanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace JdkScanner {

namespace {

QString releaseValue(const QString &text, const char *key)
{
    const QString k = QLatin1String(key) + QLatin1Char('=');
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.startsWith(k))
            continue;
        QString v = line.mid(k.size()).trimmed();
        if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')) && v.size() >= 2)
            v = v.mid(1, v.size() - 2);
        return v;
    }
    return {};
}

} // namespace

QString Jdk::label() const
{
    QString s = version.isEmpty() ? QString::number(major) : version;
    if (!vendor.isEmpty())
        s += QStringLiteral(" · ") + vendor;
    if (jre)
        s += QStringLiteral(" (JRE)");
    return s;
}

int majorOf(const QString &version)
{
    const QStringList parts = version.split(QLatin1Char('.'));
    if (parts.isEmpty())
        return 0;
    if (parts.first() == QLatin1String("1") && parts.size() > 1)
        return parts.at(1).toInt();
    QString first = parts.first();
    while (!first.isEmpty() && !first.back().isDigit())
        first.chop(1);
    return first.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('+'), 0, 0).toInt();
}

Jdk fromHome(const QString &homeIn)
{
    Jdk j;
    if (homeIn.isEmpty())
        return j;
    QString home = QFileInfo(homeIn).canonicalFilePath();
    // macOS-style bundles and Gradle's provisioned folders sometimes nest the JDK one level down.
    if (!home.isEmpty() && !QFileInfo::exists(home + QStringLiteral("/bin/java"))) {
        for (const QString &sub : QDir(home).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (QFileInfo::exists(home + QLatin1Char('/') + sub + QStringLiteral("/bin/java"))) {
                home += QLatin1Char('/') + sub;
                break;
            }
    }
    if (home.isEmpty() || !QFileInfo(home + QStringLiteral("/bin/java")).isExecutable())
        return j;
    QFile f(home + QStringLiteral("/release"));
    QString text;
    if (f.open(QIODevice::ReadOnly))
        text = QString::fromUtf8(f.read(16 * 1024));
    j.home = home;
    j.version = releaseValue(text, "JAVA_VERSION");
    j.vendor = releaseValue(text, "IMPLEMENTOR");
    if (j.version.isEmpty()) {
        // No release file: the folder name usually says it (java-17-openjdk-amd64, jdk-21.0.2+13).
        const QString name = QFileInfo(home).fileName();
        for (const QString &piece : name.split(QRegularExpression(QStringLiteral("[-_]"))))
            if (!piece.isEmpty() && piece.at(0).isDigit()) {
                j.version = piece;
                break;
            }
    }
    j.major = majorOf(j.version);
    j.jre = !QFileInfo::exists(home + QStringLiteral("/bin/javac"));
    return j;
}

QList<Jdk> scan()
{
    QList<Jdk> out;
    QSet<QString> seen;
    auto add = [&](const QString &home, const char *source) {
        Jdk j = fromHome(home);
        if (!j.isValid() || seen.contains(j.home))
            return;
        seen.insert(j.home);
        j.source = QLatin1String(source);
        out << j;
    };
    auto addChildren = [&](const QString &dir, const char *source) {
        for (const QString &sub : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (sub != QLatin1String("current") && !sub.endsWith(QLatin1String(".lock")))
                add(dir + QLatin1Char('/') + sub, source);
    };
    add(qEnvironmentVariable("JAVA_HOME"), "JAVA_HOME");
    addChildren(QDir::homePath() + QStringLiteral("/.sdkman/candidates/java"), "SDKMAN");
    addChildren(QStringLiteral("/usr/lib/jvm"), "System");
    addChildren(QStringLiteral("/usr/java"), "System");
    addChildren(QStringLiteral("/opt/java"), "System");
    addChildren(QDir::homePath() + QStringLiteral("/.gradle/jdks"), "Gradle");
    addChildren(QDir::homePath() + QStringLiteral("/.jdks"), "IntelliJ");
    addChildren(QDir::homePath() + QStringLiteral("/.local/share/mise/installs/java"), "mise");
    addChildren(QDir::homePath() + QStringLiteral("/.asdf/installs/java"), "asdf");
    const QString java = QStandardPaths::findExecutable(QStringLiteral("java"));
    if (!java.isEmpty())
        add(QFileInfo(QFileInfo(java).canonicalFilePath()).absolutePath() + QStringLiteral("/.."), "PATH");
    std::stable_sort(out.begin(), out.end(), [](const Jdk &a, const Jdk &b) { return a.major > b.major; });
    return out;
}

Jdk defaultJdk(const QList<Jdk> &all)
{
    const QString env = QFileInfo(qEnvironmentVariable("JAVA_HOME")).canonicalFilePath();
    QString path;
    if (const QString java = QStandardPaths::findExecutable(QStringLiteral("java")); !java.isEmpty())
        path = QFileInfo(QFileInfo(QFileInfo(java).canonicalFilePath()).absolutePath() + QStringLiteral("/..")).canonicalFilePath();
    for (const QString &want : {env, path})
        for (const Jdk &j : all)
            if (!want.isEmpty() && j.home == want)
                return j;
    return {};
}

} // namespace JdkScanner
