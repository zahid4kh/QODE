#include "project/JvmProject.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace {

struct Main {
    QString cls;  // fully qualified class to start
    bool kotlin = false;
};

struct Module {
    QString dir;   // "" = the project root, else the folder name
    QString build; // text of build.gradle(.kts) / pom.xml
    bool kts = false;
    bool kotlin = false, compose = false, android = false, spring = false, quarkus = false, micronaut = false,
         ktor = false, appPlugin = false, javafx = false;
    QString mainClass; // declared in the build file
    QList<Main> mains; // found in the sources
};

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

bool has(const QString &text, const char *pattern, QRegularExpression::PatternOption o = QRegularExpression::NoPatternOption)
{
    return QRegularExpression(QLatin1String(pattern), o).match(text).hasMatch();
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

// Finds `main` entry points under the usual source folders of the module.
QList<Main> scanMains(const QString &moduleDir)
{
    static const QRegularExpression javaMain(QStringLiteral(R"(\bstatic\s+void\s+main\s*\()"));
    static const QRegularExpression ktMain(QStringLiteral(R"(^[ \t]*(?:@JvmStatic\s+)?(?:suspend\s+)?fun\s+main\s*\()"),
                                           QRegularExpression::MultilineOption);
    static const QRegularExpression pkgRe(QStringLiteral(R"(^\s*package\s+([\w.]+))"), QRegularExpression::MultilineOption);
    static const QRegularExpression jvmName(QStringLiteral(R"re(@file:JvmName\(\s*"(\w+)"\s*\))re"));
    static const QRegularExpression objRe(QStringLiteral(R"(\b(?:object|class)\s+(\w+))"));

    QList<Main> out;
    int files = 0;
    for (const char *sub : {"src/main/java", "src/main/kotlin", "src/jvmMain/kotlin", "src/desktopMain/kotlin", "src/main"}) {
        const QString base = moduleDir + QLatin1Char('/') + QLatin1String(sub);
        if (!QFileInfo(base).isDir())
            continue;
        const bool wide = QLatin1String(sub) == QLatin1String("src/main");
        QDirIterator it(base, {QStringLiteral("*.java"), QStringLiteral("*.kt")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext() && files < 600) {
            const QString path = it.next();
            if (wide && (path.contains(QStringLiteral("/src/main/java/")) || path.contains(QStringLiteral("/src/main/kotlin/"))))
                continue; // already covered by the specific folder
            ++files;
            const QString text = readText(path, 128 * 1024);
            const bool kt = path.endsWith(QLatin1String(".kt"));
            const QRegularExpressionMatch m = (kt ? ktMain : javaMain).match(text);
            if (!m.hasMatch())
                continue;
            const QString pkg = pkgRe.match(text).captured(1);
            const QString stem = QFileInfo(path).completeBaseName();
            QString cls;
            if (!kt) {
                cls = stem;
            } else if (m.captured(0).contains(QStringLiteral("@JvmStatic"))) {
                // object Foo { @JvmStatic fun main } -> Foo
                const QString head = text.left(m.capturedStart());
                QRegularExpressionMatchIterator oi = objRe.globalMatch(head);
                QString last;
                while (oi.hasNext())
                    last = oi.next().captured(1);
                cls = last.isEmpty() ? stem : last;
            } else if (const QString jn = jvmName.match(text).captured(1); !jn.isEmpty()) {
                cls = jn;
            } else {
                cls = stem.left(1).toUpper() + stem.mid(1) + QStringLiteral("Kt");
            }
            out.append({pkg.isEmpty() ? cls : pkg + QLatin1Char('.') + cls, kt});
        }
    }
    return out;
}

void analyseGradle(Module &m)
{
    // Plugins declared `apply false` in a root build file only pin versions for the sub-modules.
    QString t = m.build;
    t.remove(QRegularExpression(QStringLiteral(R"(^.*apply\s+false.*$)"), QRegularExpression::MultilineOption));
    m.kotlin = has(t, R"(kotlin\s*\(|org\.jetbrains\.kotlin|kotlin-jvm|libs\.plugins\.kotlin|kotlin\("jvm"\)|kotlin-gradle-plugin)");
    m.compose = has(t, R"(compose\.desktop|org\.jetbrains\.compose|jetbrainsCompose|composeMultiplatform|compose\.multiplatform|libs\.plugins\.compose)")
                && has(t, R"(\bdesktop\b)");
    m.android = has(t, R"(com\.android\.application|android\.application|androidApplication)");
    m.spring = has(t, R"(org\.springframework\.boot)");
    m.quarkus = has(t, R"(io\.quarkus)");
    m.micronaut = has(t, R"(io\.micronaut\.application)");
    m.ktor = has(t, R"(io\.ktor\.plugin)");
    m.javafx = has(t, R"(org\.openjfx\.javafxplugin)");
    m.appPlugin = has(t, R"re((id\s*\(\s*["']application["']\s*\)|^\s*`?application`?\s*$|apply\s+plugin:\s*["']application["']|\bapplication\s*\{))re",
                      QRegularExpression::MultilineOption)
                  || m.compose || m.javafx;
    static const QRegularExpression mc(QStringLiteral(R"re(mainClass(?:Name)?\s*(?:\.set\s*\(|=)\s*["']([\w.$]+)["'])re"));
    m.mainClass = mc.match(t).captured(1);
}

void analyseMaven(Module &m)
{
    const QString &t = m.build;
    m.kotlin = t.contains(QStringLiteral("kotlin-maven-plugin"));
    m.spring = t.contains(QStringLiteral("spring-boot-maven-plugin"));
    m.quarkus = t.contains(QStringLiteral("quarkus-maven-plugin"));
    m.javafx = t.contains(QStringLiteral("javafx-maven-plugin"));
    static const QRegularExpression mc(QStringLiteral(R"(<(?:exec\.mainClass|mainClass|start-class)>\s*([\w.$]+)\s*<)"));
    m.mainClass = mc.match(t).captured(1);
}

} // namespace

JvmProject JvmProject::detect(const QString &root)
{
    JvmProject p;
    if (root.isEmpty())
        return p;

    const bool wrapper = QFileInfo::exists(root + QStringLiteral("/gradlew"));
    const bool gradle = wrapper || !gradleFile(root).isEmpty() || QFileInfo::exists(root + QStringLiteral("/settings.gradle.kts"))
                        || QFileInfo::exists(root + QStringLiteral("/settings.gradle"));
    const bool maven = !gradle && QFileInfo::exists(root + QStringLiteral("/pom.xml"));
    if (!gradle && !maven)
        return p;

    // The root and (multi-module builds, e.g. composeApp/, desktopApp/, app/) its direct sub-folders.
    QList<Module> modules;
    auto addModule = [&](const QString &dir) {
        const QString abs = dir.isEmpty() ? root : root + QLatin1Char('/') + dir;
        const QString f = gradle ? gradleFile(abs) : abs + QStringLiteral("/pom.xml");
        if (f.isEmpty() || !QFileInfo::exists(f))
            return;
        Module m;
        m.dir = dir;
        m.build = readText(f);
        m.kts = f.endsWith(QLatin1String(".kts"));
        gradle ? analyseGradle(m) : analyseMaven(m);
        m.mains = scanMains(abs);
        if (!m.kotlin && !m.mains.isEmpty())
            m.kotlin = m.mains.first().kotlin;
        modules.append(m);
    };
    addModule(QString());
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (d.startsWith(QLatin1Char('.')) || d == QLatin1String("build") || d == QLatin1String("buildSrc") || d == QLatin1String("gradle")
            || d == QLatin1String("node_modules") || d == QLatin1String("target") || d == QLatin1String("src"))
            continue;
        addModule(d);
    }
    if (modules.isEmpty())
        return p;

    QString tool;
    if (gradle) {
        tool = wrapper ? (QFileInfo(root + QStringLiteral("/gradlew")).isExecutable() ? QStringLiteral("./gradlew") : QStringLiteral("sh gradlew"))
                       : QStringLiteral("gradle");
    } else {
        const QString w = root + QStringLiteral("/mvnw");
        tool = QFileInfo::exists(w) ? (QFileInfo(w).isExecutable() ? QStringLiteral("./mvnw") : QStringLiteral("sh mvnw")) : QStringLiteral("mvn");
    }

    struct Scored { int score; Config c; QString kind; };
    QList<Scored> runs;
    QString initScript; // created lazily, only when a module needs it

    auto ensureInit = [&]() {
        if (!initScript.isEmpty())
            return initScript;
        // Runs any main class of a module that has no `application` plugin (Java and Kotlin/JVM alike); lives in the
        // cache, so the project stays untouched.
        const QString dir = QDir::homePath() + QStringLiteral("/.cache/QODE/run");
        QDir().mkpath(dir);
        initScript = dir + QStringLiteral("/run-main.init.gradle");
        const QByteArray body =
            "// Written by QODE: lets the Run button start a main class without the `application` plugin.\n"
            "allprojects { p ->\n"
            "    p.plugins.withId('java') {\n"
            "        if (p.findProperty('qode.main') != null && p.tasks.findByName('qodeRun') == null) {\n"
            "            p.tasks.register('qodeRun', JavaExec) {\n"
            "                group = 'application'\n"
            "                classpath = p.sourceSets.main.runtimeClasspath\n"
            "                mainClass = p.property('qode.main')\n"
            "                standardInput = System.in\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "}\n";
        QFile cur(initScript);
        if (!cur.open(QIODevice::ReadOnly) || cur.readAll() != body) {
            cur.close();
            QSaveFile out(initScript);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(body);
                out.commit();
            }
        }
        return initScript;
    };

    auto kindOf = [&](const Module &m, const QString &framework) {
        QStringList parts;
        parts << (m.kotlin ? QStringLiteral("Kotlin") : QStringLiteral("Java"));
        parts << (gradle ? (m.kts ? QStringLiteral("Gradle (Kotlin DSL)") : QStringLiteral("Gradle (Groovy DSL)")) : QStringLiteral("Maven"));
        if (!framework.isEmpty())
            parts << framework;
        return parts.join(QStringLiteral(" · "));
    };

    for (const Module &m : modules) {
        const QString where = m.dir.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m.dir);
        auto add = [&](int score, const QString &name, const QString &cmd, const QString &framework) {
            runs.append({score, {name + where, cmd, true}, kindOf(m, framework)});
        };
        if (gradle) {
            const QString pre = m.dir.isEmpty() ? QString() : QLatin1Char(':') + m.dir + QLatin1Char(':');
            const QString base = tool + QStringLiteral(" --console=plain ");
            if (m.android) {
                add(40, QStringLiteral("Install debug build on a device"), base + pre + QStringLiteral("installDebug"), QStringLiteral("Android"));
                continue;
            }
            if (m.compose) {
                add(100, QStringLiteral("Run Compose Desktop app"), tool + QLatin1Char(' ') + pre + QStringLiteral("run"), QStringLiteral("Compose Desktop"));
                // Compose 1.10+ bundles hot reload; the task needs the main class spelled out.
                if (!m.mainClass.isEmpty())
                    add(99, QStringLiteral("Run Compose Desktop app with hot reload"),
                        tool + QLatin1Char(' ') + (pre.isEmpty() ? QStringLiteral(":") : pre) + QStringLiteral("hotRun --mainClass ")
                            + (m.mainClass.contains(QLatin1Char('$')) ? shellQuote(m.mainClass) : m.mainClass) + QStringLiteral(" --auto"),
                        QStringLiteral("Compose Desktop"));
            } else if (m.spring) {
                add(90, QStringLiteral("Run Spring Boot app"), base + pre + QStringLiteral("bootRun"), QStringLiteral("Spring Boot"));
            } else if (m.quarkus) {
                add(90, QStringLiteral("Run Quarkus dev mode"), base + pre + QStringLiteral("quarkusDev"), QStringLiteral("Quarkus"));
            } else if (m.appPlugin && (!m.mainClass.isEmpty() || m.ktor || m.micronaut || m.mains.isEmpty())) {
                const QString fw = m.ktor ? QStringLiteral("Ktor") : m.micronaut ? QStringLiteral("Micronaut") : m.javafx ? QStringLiteral("JavaFX") : QString();
                add(80, QStringLiteral("Run application"), base + pre + QStringLiteral("run"), fw);
            } else if (!m.mains.isEmpty()) {
                // No `application` plugin (or no mainClass): run a found main through the init script.
                int n = 0;
                for (const Main &mn : m.mains) {
                    if (++n > 6)
                        break;
                    const QString simple = mn.cls.mid(mn.cls.lastIndexOf(QLatin1Char('.')) + 1);
                    add(60 - n, QStringLiteral("Run %1").arg(simple),
                        base + QStringLiteral("--init-script ") + shellQuote(ensureInit()) + QStringLiteral(" -Pqode.main=") + shellQuote(mn.cls)
                            + QLatin1Char(' ') + pre + QStringLiteral("qodeRun"),
                        QString());
                }
            }
        } else {
            const QString base = tool + (m.dir.isEmpty() ? QString() : QStringLiteral(" -f ") + shellQuote(m.dir + QStringLiteral("/pom.xml")));
            if (m.spring) {
                add(90, QStringLiteral("Run Spring Boot app"), base + QStringLiteral(" spring-boot:run"), QStringLiteral("Spring Boot"));
            } else if (m.quarkus) {
                add(90, QStringLiteral("Run Quarkus dev mode"), base + QStringLiteral(" quarkus:dev"), QStringLiteral("Quarkus"));
            } else if (m.javafx) {
                add(85, QStringLiteral("Run JavaFX app"), base + QStringLiteral(" javafx:run"), QStringLiteral("JavaFX"));
            } else if (!m.mainClass.isEmpty()) {
                add(80, QStringLiteral("Run %1").arg(m.mainClass.mid(m.mainClass.lastIndexOf(QLatin1Char('.')) + 1)),
                    base + QStringLiteral(" -q compile exec:java -Dexec.mainClass=") + shellQuote(m.mainClass), QString());
            } else {
                int n = 0;
                for (const Main &mn : m.mains) {
                    if (++n > 6)
                        break;
                    add(60 - n, QStringLiteral("Run %1").arg(mn.cls.mid(mn.cls.lastIndexOf(QLatin1Char('.')) + 1)),
                        base + QStringLiteral(" -q compile exec:java -Dexec.mainClass=") + shellQuote(mn.cls), QString());
                }
            }
        }
    }
    if (runs.isEmpty())
        return p;

    std::stable_sort(runs.begin(), runs.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
    for (const Scored &s : runs)
        p.m_configs.append(s.c);
    p.m_kind = runs.first().kind;

    // Test / build helpers for the same build.
    if (gradle) {
        p.m_configs.append({QStringLiteral("Run tests"), tool + QStringLiteral(" --console=plain test"), false});
        p.m_configs.append({QStringLiteral("Build"), tool + QStringLiteral(" --console=plain build"), false});
    } else {
        p.m_configs.append({QStringLiteral("Run tests"), tool + QStringLiteral(" test"), false});
        p.m_configs.append({QStringLiteral("Build"), tool + QStringLiteral(" clean package"), false});
    }
    p.m_valid = true;
    return p;
}
