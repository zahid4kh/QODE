#include "jvm/JdkScanner.h"
#include "jvm/JdkInstaller.h"
#include "jvm/JvmTools.h"
#include "platform/AppPaths.h"
#include "platform/Platform.h"

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

QString toolIn(const QString &home, const char *tool) // <home>/bin/java (java.exe on Windows)
{
    return home + QStringLiteral("/bin/") + QLatin1String(tool) + Platform::exeSuffix();
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
    QString home = Platform::realPath(homeIn); // Windows: through junctions too
    // macOS-style bundles and Gradle's provisioned folders sometimes nest the JDK one level down.
    if (!home.isEmpty() && !QFileInfo::exists(toolIn(home, "java"))) {
        for (const QString &sub : QDir(home).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (QFileInfo::exists(toolIn(home + QLatin1Char('/') + sub, "java"))) {
                // Windows: Oracle's Java\latest\jdk-25 is a junction, which only realPath follows. Elsewhere the
                // nested path is kept as it is.
                const QString nested = home + QLatin1Char('/') + sub, real = Platform::realPath(nested);
                home = real == QFileInfo(nested).canonicalFilePath() ? nested : real;
                break;
            }
    }
    if (home.isEmpty() || !QFileInfo(toolIn(home, "java")).isExecutable())
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
    j.jre = !QFileInfo::exists(toolIn(home, "javac"));
    return j;
}

QList<Jdk> scan()
{
    QList<Jdk> out;
    QSet<QString> seen;
    auto add = [&](const QString &home, const QString &source) {
        Jdk j = fromHome(home);
        if (!j.isValid() || seen.contains(Platform::pathKey(j.home))) // Windows: the same folder in another case
            return;
        seen.insert(Platform::pathKey(j.home));
        j.source = source;
        out << j;
    };
    auto addChildren = [&](const QString &dir, const QString &source) {
        for (const QString &sub : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (sub != QLatin1String("current") && !sub.endsWith(QLatin1String(".lock")))
                add(dir + QLatin1Char('/') + sub, source);
    };
    add(qEnvironmentVariable("JAVA_HOME"), QStringLiteral("JAVA_HOME"));
    addChildren(QDir::homePath() + QStringLiteral("/.sdkman/candidates/java"), QStringLiteral("SDKMAN"));
    addChildren(QStringLiteral("/usr/lib/jvm"), QStringLiteral("System"));
    addChildren(QStringLiteral("/usr/java"), QStringLiteral("System"));
    addChildren(QStringLiteral("/opt/java"), QStringLiteral("System"));
    addChildren(QDir::homePath() + QStringLiteral("/.gradle/jdks"), QStringLiteral("Gradle"));
    addChildren(QDir::homePath() + QStringLiteral("/.jdks"), QStringLiteral("IntelliJ"));
    if (AppPaths::isPortable()) // portable QODE downloads JDKs into its own folder
        addChildren(JdkInstaller::defaultRoot(), QStringLiteral("QODE"));
    addChildren(QDir::homePath() + QStringLiteral("/.local/share/mise/installs/java"), QStringLiteral("mise"));
    addChildren(QDir::homePath() + QStringLiteral("/.asdf/installs/java"), QStringLiteral("asdf"));
    for (const Platform::JdkPlace &p : Platform::jdkFolders()) // Windows: Program Files\<vendor>
        addChildren(p.path, p.source);
    for (const Platform::JdkPlace &p : Platform::jdkHomes()) // Windows: the registry, Scoop
        add(p.path, p.source);
    const QString java = QStandardPaths::findExecutable(QStringLiteral("java"));
    if (!java.isEmpty())
        add(QFileInfo(Platform::realPath(java)).absolutePath() + QStringLiteral("/.."), QStringLiteral("PATH"));
    std::stable_sort(out.begin(), out.end(), [](const Jdk &a, const Jdk &b) { return a.major > b.major; });
    return out;
}

Jdk defaultJdk(const QList<Jdk> &all)
{
    const QString env = Platform::realPath(qEnvironmentVariable("JAVA_HOME"));
    QString path;
    if (const QString java = QStandardPaths::findExecutable(QStringLiteral("java")); !java.isEmpty())
        path = Platform::realPath(QFileInfo(Platform::realPath(java)).absolutePath() + QStringLiteral("/.."));
    for (const QString &want : {env, path})
        for (const Jdk &j : all)
            if (!want.isEmpty() && Platform::samePath(j.home, want))
                return j;
    return {};
}

Jdk forGradle(const QList<Jdk> &all, const QString &gradleVersion, const QString &chosenHome)
{
    const Jdk chosen = chosenHome.isEmpty() ? Jdk() : fromHome(chosenHome);
    const Jdk system = defaultJdk(all);
    for (const Jdk &j : {chosen, system})
        if (j.isValid() && JvmTools::gradleRunsOn(gradleVersion, j.major))
            return j;
    if (gradleVersion.isEmpty())
        return {};
    // `all` is sorted newest first: the newest JDK (not a JRE) this Gradle runs on.
    for (const Jdk &j : all)
        if (!j.jre && JvmTools::gradleRunsOn(gradleVersion, j.major))
            return j;
    for (const Jdk &j : all)
        if (JvmTools::gradleRunsOn(gradleVersion, j.major))
            return j;
    return {};
}

Needs needsOf(const QString &root, const QList<Jdk> &all, const QString &chosenHome)
{
    Needs n;
    if (!JvmTools::isJvmBuild(root))
        return n;
    const JvmTools::Versions v = JvmTools::detectVersions(root);
    if (v.maven)
        return n;
    n.isGradle = true;
    n.gradle = JvmTools::wrapperVersion(root).value;
    n.runner = forGradle(all, n.gradle, chosenHome);
    n.toolchain = v.toolchain;
    if (n.toolchain > 0 && !v.autoProvision) {
        n.toolchainMissing = true;
        for (const Jdk &j : all)
            if (j.major == n.toolchain && !j.jre)
                n.toolchainMissing = false;
        // Gradle also takes the JVM it runs on as a toolchain.
        if (n.runner.isValid() && n.runner.major == n.toolchain)
            n.toolchainMissing = false;
    }
    if (n.toolchainMissing)
        n.downloads << n.toolchain;
    if (!n.runner.isValid() && !(n.toolchainMissing && JvmTools::gradleRunsOn(n.gradle, n.toolchain))) {
        // The newest long-term-support release this Gradle runs on.
        for (int lts : {25, 21, 17, 11, 8})
            if (JvmTools::gradleRunsOn(n.gradle, lts)) {
                n.downloads << lts;
                break;
            }
    }
    return n;
}

} // namespace JdkScanner
