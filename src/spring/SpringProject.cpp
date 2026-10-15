#include "spring/SpringProject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace {

QString readText(const QString &path, qint64 max = 256 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(max));
}

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString gradleFile(const QString &dir)
{
    for (const char *name : {"build.gradle.kts", "build.gradle"}) {
        const QString p = dir + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

// `alias(libs.plugins.spring.boot)` -> the plugin id the version catalog gives it ("org.springframework.boot").
bool catalogHasSpringBoot(const QString &dir, const QString &build)
{
    static const QRegularExpression alias(QStringLiteral(R"(libs\.plugins\.([\w.]+))"));
    QSet<QString> used;
    for (QRegularExpressionMatchIterator it = alias.globalMatch(build); it.hasNext();)
        used.insert(it.next().captured(1));
    if (used.isEmpty())
        return false;
    // The catalog sits next to the build or in a parent folder (multi-module builds).
    QDir d(dir);
    for (int i = 0; i < 3; ++i) {
        const QString toml = d.filePath(QStringLiteral("gradle/libs.versions.toml"));
        if (QFileInfo::exists(toml)) {
            const QString text = readText(toml);
            bool inPlugins = false;
            for (const QString &raw : text.split(QLatin1Char('\n'))) {
                const QString line = raw.trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    inPlugins = line.startsWith(QStringLiteral("[plugins]"));
                    continue;
                }
                if (!inPlugins || !line.contains(QStringLiteral("org.springframework.boot")))
                    continue;
                QString name = line.left(line.indexOf(QLatin1Char('='))).trimmed();
                name.replace(QLatin1Char('-'), QLatin1Char('.')).replace(QLatin1Char('_'), QLatin1Char('.'));
                if (used.contains(name))
                    return true;
            }
            return false;
        }
        if (!d.cdUp())
            break;
    }
    return false;
}

QString applicationName(const QString &dir)
{
    static const QRegularExpression props(QStringLiteral(R"(^\s*spring\.application\.name\s*[=:]\s*(\S+))"), QRegularExpression::MultilineOption);
    static const QRegularExpression yml(QStringLiteral(R"(^\s*name:\s*([\w.-]+)\s*$)"), QRegularExpression::MultilineOption);
    const QString res = dir + QStringLiteral("/src/main/resources/");
    QString n = props.match(readText(res + QStringLiteral("application.properties"), 64 * 1024)).captured(1);
    if (n.isEmpty()) {
        for (const char *f : {"application.yml", "application.yaml"}) {
            const QString t = readText(res + QLatin1String(f), 64 * 1024);
            // Only the `name:` under `spring: application:` counts; a cheap check is enough for a label.
            if (t.contains(QStringLiteral("application:")) && t.contains(QStringLiteral("spring:")))
                n = yml.match(t).captured(1);
            if (!n.isEmpty())
                break;
        }
    }
    return n;
}

bool isSpringModule(const QString &dir, bool *maven)
{
    const QString g = gradleFile(dir);
    if (!g.isEmpty()) {
        const QString t = readText(g);
        if (t.contains(QStringLiteral("org.springframework.boot")) || catalogHasSpringBoot(dir, t)) {
            *maven = false;
            return true;
        }
    }
    const QString pom = dir + QStringLiteral("/pom.xml");
    if (QFileInfo::exists(pom)) {
        const QString t = readText(pom);
        if (t.contains(QStringLiteral("spring-boot-maven-plugin")) || t.contains(QStringLiteral("spring-boot-starter-parent"))) {
            *maven = true;
            return true;
        }
    }
    return false;
}

} // namespace

SpringProject SpringProject::detect(const QString &root)
{
    SpringProject p;
    if (root.isEmpty())
        return p;
    QStringList candidates{QDir(root).absolutePath()};
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (d.startsWith(QLatin1Char('.')) || d == QLatin1String("build") || d == QLatin1String("buildSrc") || d == QLatin1String("gradle")
            || d == QLatin1String("node_modules") || d == QLatin1String("target") || d == QLatin1String("src") || d == QLatin1String("out"))
            continue;
        candidates << QDir(root).filePath(d);
    }
    for (const QString &dir : candidates) {
        bool maven = false;
        if (!isSpringModule(dir, &maven))
            continue;
        p.valid = true;
        p.maven = maven;
        p.moduleDir = dir;
        // The wrapper / settings file / parent pom of a multi-module build lives in the root.
        const QString top = QDir(root).absolutePath();
        const bool rootBuilds = maven ? QFileInfo::exists(top + QStringLiteral("/pom.xml"))
                                      : (QFileInfo::exists(top + QStringLiteral("/gradlew")) || QFileInfo::exists(top + QStringLiteral("/settings.gradle.kts"))
                                         || QFileInfo::exists(top + QStringLiteral("/settings.gradle")));
        p.workDir = rootBuilds ? top : dir;
        if (maven) {
            const QString w = p.workDir + QStringLiteral("/mvnw");
            p.tool = QFileInfo::exists(w) ? (QFileInfo(w).isExecutable() ? QStringLiteral("./mvnw") : QStringLiteral("sh mvnw")) : QStringLiteral("mvn");
        } else {
            const QString w = p.workDir + QStringLiteral("/gradlew");
            p.tool = QFileInfo::exists(w) ? (QFileInfo(w).isExecutable() ? QStringLiteral("./gradlew") : QStringLiteral("sh gradlew")) : QStringLiteral("gradle");
        }
        p.appName = applicationName(dir);
        if (p.appName.isEmpty())
            p.appName = QFileInfo(dir).fileName();
        return p;
    }
    return p;
}

QString SpringProject::command(const Options &o) const
{
    if (!o.command.trimmed().isEmpty())
        return o.command.trimmed();
    if (!valid)
        return {};
    QStringList args;
    if (!o.profiles.trimmed().isEmpty()) {
        QString profiles = o.profiles;
        profiles.remove(QLatin1Char(' '));
        args << QStringLiteral("--spring.profiles.active=") + profiles;
    }
    if (o.port > 0)
        args << QStringLiteral("--server.port=%1").arg(o.port);
    if (!o.arguments.trimmed().isEmpty())
        args << o.arguments.trimmed();
    const QString joined = args.join(QLatin1Char(' '));
    const QString jvm = o.jvmArgs.trimmed();

    if (maven) {
        QString cmd = tool;
        if (moduleDir != workDir)
            cmd += QStringLiteral(" -f ") + shellQuote(QDir(workDir).relativeFilePath(moduleDir) + QStringLiteral("/pom.xml"));
        cmd += QStringLiteral(" spring-boot:run");
        if (!joined.isEmpty())
            cmd += QStringLiteral(" -Dspring-boot.run.arguments=") + shellQuote(joined);
        if (!jvm.isEmpty())
            cmd += QStringLiteral(" -Dspring-boot.run.jvmArguments=") + shellQuote(jvm);
        return cmd;
    }
    QString prefix;
    if (moduleDir != workDir)
        prefix = QLatin1Char(':') + QDir(workDir).relativeFilePath(moduleDir).replace(QLatin1Char('/'), QLatin1Char(':')) + QLatin1Char(':');
    QString cmd;
    // bootRun forks the application with the build's environment, so the JVM options travel in JAVA_TOOL_OPTIONS
    // (the JVM prints a "Picked up" line for it).
    if (!jvm.isEmpty())
        cmd = QStringLiteral("JAVA_TOOL_OPTIONS=") + shellQuote(jvm) + QLatin1Char(' ');
    cmd += tool + QStringLiteral(" --console=plain ") + prefix + QStringLiteral("bootRun");
    if (!joined.isEmpty())
        cmd += QStringLiteral(" --args=") + shellQuote(joined);
    return cmd;
}

QStringList SpringProject::sourceRoots() const
{
    QStringList out;
    if (!valid)
        return out;
    for (const char *sub : {"src/main/kotlin", "src/main/java", "src/test/kotlin", "src/test/java"}) {
        const QString d = moduleDir + QLatin1Char('/') + QLatin1String(sub);
        if (QFileInfo(d).isDir())
            out << d;
    }
    return out;
}

QString SpringProject::resourcesDir() const
{
    return valid ? moduleDir + QStringLiteral("/src/main/resources") : QString();
}

QString SpringProject::buildFile() const
{
    if (!valid)
        return {};
    if (maven)
        return moduleDir + QStringLiteral("/pom.xml");
    return gradleFile(moduleDir);
}

bool SpringProject::hasActuator() const
{
    const QString f = buildFile();
    if (f.isEmpty())
        return false;
    if (readText(f).contains(QStringLiteral("spring-boot-starter-actuator")))
        return true;
    // `libs.spring.boot.starter.actuator` through the version catalog.
    static const QRegularExpression accessor(QStringLiteral(R"(libs\.[\w.]*actuator)"));
    if (!accessor.match(readText(f)).hasMatch())
        return false;
    QDir d(moduleDir);
    for (int i = 0; i < 3; ++i) {
        const QString toml = d.filePath(QStringLiteral("gradle/libs.versions.toml"));
        if (QFileInfo::exists(toml))
            return readText(toml).contains(QStringLiteral("spring-boot-starter-actuator"));
        if (!d.cdUp())
            break;
    }
    return false;
}

QStringList SpringProject::profiles() const
{
    static const QRegularExpression re(QStringLiteral(R"(^application-(.+)\.(?:properties|ya?ml)$)"));
    QStringList out;
    for (const QString &name : QDir(resourcesDir()).entryList(QDir::Files, QDir::Name)) {
        const QRegularExpressionMatch m = re.match(name);
        if (m.hasMatch() && !out.contains(m.captured(1)))
            out << m.captured(1);
    }
    return out;
}

QStringList SpringProject::parseEnv(const QString &text)
{
    QStringList out;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || !line.contains(QLatin1Char('=')) || line.startsWith(QLatin1Char('=')))
            continue;
        out << line;
    }
    return out;
}
