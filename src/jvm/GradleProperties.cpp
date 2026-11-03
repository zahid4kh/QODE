#include "jvm/GradleProperties.h"

#include "jvm/JdkScanner.h"
#include "jvm/JvmTools.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace GradleProperties {

namespace {

struct Key {
    const char *name;
    const char *detail;
    const char *values; // "a|b|c"; "" = free text; "@jdk" / "@sdk" / "@ndk" / "@cmake" / "@dist" = looked up
};

const Key kGradleKeys[] = {
    {"org.gradle.jvmargs", "JVM options of the Gradle daemon", "-Xmx2g -Dfile.encoding=UTF-8|-Xmx4g -XX:MaxMetaspaceSize=1g -Dfile.encoding=UTF-8"},
    {"org.gradle.parallel", "Build independent projects in parallel (false)", "true|false"},
    {"org.gradle.caching", "Reuse task outputs from the build cache (false)", "true|false"},
    {"org.gradle.caching.debug", "Log the build cache key of every task (false)", "true|false"},
    {"org.gradle.daemon", "Run builds in the Gradle daemon (true)", "true|false"},
    {"org.gradle.daemon.idletimeout", "Milliseconds an idle daemon stays alive (10800000)", "10800000|3600000"},
    {"org.gradle.configureondemand", "Configure only the projects a build needs (false, incubating)", "true|false"},
    {"org.gradle.configuration-cache", "Cache the result of the configuration phase (false)", "true|false"},
    {"org.gradle.configuration-cache.problems", "What configuration cache problems do (fail)", "fail|warn"},
    {"org.gradle.configuration-cache.parallel", "Load and store the configuration cache in parallel (false)", "true|false"},
    {"org.gradle.configuration-cache.max-problems", "Problems before the build fails (512)", "512"},
    {"org.gradle.unsafe.isolated-projects", "Isolated projects (incubating)", "true|false"},
    {"org.gradle.console", "Console output style", "auto|plain|rich|verbose|colored"},
    {"org.gradle.logging.level", "Log level", "quiet|warn|lifecycle|info|debug"},
    {"org.gradle.logging.stacktrace", "Stack traces of failures", "internal|all|full"},
    {"org.gradle.warning.mode", "How deprecation warnings show", "all|fail|summary|none"},
    {"org.gradle.workers.max", "Most workers Gradle uses (the CPU count)", "4|8"},
    {"org.gradle.priority", "Scheduling priority of the daemon", "low|normal"},
    {"org.gradle.java.home", "JDK that runs Gradle", "@jdk"},
    {"org.gradle.java.installations.paths", "Extra JDK folders for toolchains (comma-separated)", "@jdk"},
    {"org.gradle.java.installations.auto-detect", "Find toolchains on this machine (true)", "true|false"},
    {"org.gradle.java.installations.auto-download", "Download missing toolchains (true)", "true|false"},
    {"org.gradle.vfs.watch", "Watch the file system between builds (true)", "true|false"},
    {"org.gradle.vfs.verbose", "Log file-system watching (false)", "true|false"},
    {"org.gradle.debug", "Debug the build itself on port 5005 (false)", "true|false"},
    {"org.gradle.debug.port", "Port of org.gradle.debug (5005)", "5005"},
    {"org.gradle.continue", "Keep going after a task fails (false)", "true|false"},
    {"org.gradle.dependency.verification", "Dependency verification mode", "strict|lenient|off"},
    {"org.gradle.welcome", "Welcome message of a new Gradle version", "once|never"},
    {"org.gradle.projectcachedir", "Project cache folder (.gradle)", ""},
    {"org.gradle.native", "Use native integrations (true)", "true|false"},
    {"org.gradle.kotlin.dsl.allWarningsAsErrors", "Kotlin DSL warnings fail the build", "true|false"},
    {"kotlin.code.style", "Kotlin code style of the project", "official|obsolete"},
    {"kotlin.incremental", "Incremental Kotlin/JVM compilation (true)", "true|false"},
    {"kotlin.incremental.js", "Incremental Kotlin/JS compilation", "true|false"},
    {"kotlin.daemon.jvmargs", "JVM options of the Kotlin daemon", "-Xmx2g"},
    {"kotlin.compiler.execution.strategy", "Where kotlinc runs", "daemon|in-process|out-of-process"},
    {"kotlin.jvm.target.validation.mode", "Mismatching Java / Kotlin JVM targets", "error|warning|ignore"},
    {"kotlin.stdlib.default.dependency", "Add the standard library automatically (true)", "true|false"},
    {"kotlin.build.report.output", "Build reports", "file|build_scan|http|json"},
    {"kotlin.mpp.stability.nowarn", "Hide the Multiplatform stability warning", "true|false"},
    {"kotlin.mpp.enableCInteropCommonization", "Commonize cinterop libraries", "true|false"},
    {"kotlin.mpp.androidSourceSetLayoutVersion", "Android source set layout", "2"},
    {"kotlin.native.ignoreDisabledTargets", "No warning for targets this host cannot build", "true|false"},
    {"kotlin.native.binary.memoryModel", "Kotlin/Native memory model", "experimental|strict"},
    {"kotlin.js.compiler", "Kotlin/JS compiler", "ir"},
    {"kapt.incremental.apt", "Incremental annotation processing", "true|false"},
    {"kapt.use.worker.api", "Run kapt in Gradle workers", "true|false"},
    {"kapt.include.compile.classpath", "Find processors on the compile class path", "true|false"},
    {"ksp.incremental", "Incremental KSP processing (true)", "true|false"},
    {"ksp.useKSP2", "Use KSP2", "true|false"},
    {"android.useAndroidX", "Use AndroidX libraries", "true|false"},
    {"android.enableJetifier", "Rewrite support-library dependencies to AndroidX", "true|false"},
    {"android.nonTransitiveRClass", "Each library's R class holds only its own resources", "true|false"},
    {"android.nonFinalResIds", "Resource IDs in R classes are not final", "true|false"},
    {"android.defaults.buildfeatures.buildconfig", "Generate BuildConfig in every module", "true|false"},
    {"android.suppressUnsupportedCompileSdk", "Compile SDKs the plugin was not tested with", ""},
    {"android.builder.sdkDownload", "Download missing SDK packages", "true|false"},
    {"android.experimental.enableTestFixturesKotlinSupport", "Kotlin in test fixtures", "true|false"},
    {"systemProp.file.encoding", "Default encoding of the build JVM", "UTF-8"},
    {"systemProp.http.proxyHost", "HTTP proxy for Gradle", ""},
    {"systemProp.http.proxyPort", "HTTP proxy port", "8080|3128"},
    {"systemProp.http.nonProxyHosts", "Hosts reached without the proxy", "localhost|127.0.0.1"},
    {"systemProp.https.proxyHost", "HTTPS proxy for Gradle", ""},
    {"systemProp.https.proxyPort", "HTTPS proxy port", "8080|3128"},
    {"systemProp.sonar.host.url", "SonarQube server", ""},
};

const Key kLocalKeys[] = {
    {"sdk.dir", "Android SDK folder", "@sdk"},
    {"ndk.dir", "Android NDK folder (deprecated: ndkVersion in the build)", "@ndk"},
    {"cmake.dir", "CMake folder of the Android SDK", "@cmake"},
};

const Key kWrapperKeys[] = {
    {"distributionUrl", "The Gradle distribution the wrapper downloads", "@dist"},
    {"distributionBase", "Base of distributionPath", "GRADLE_USER_HOME|PROJECT"},
    {"distributionPath", "Where distributions are unpacked", "wrapper/dists"},
    {"distributionSha256Sum", "Expected SHA-256 of the distribution", ""},
    {"zipStoreBase", "Base of zipStorePath", "GRADLE_USER_HOME|PROJECT"},
    {"zipStorePath", "Where the downloaded zip is kept", "wrapper/dists"},
    {"networkTimeout", "Download timeout in milliseconds (10000)", "10000"},
    {"validateDistributionUrl", "Check that distributionUrl answers (true)", "true|false"},
};

template<size_t N>
QList<const Key *> listOf(const Key (&keys)[N])
{
    QList<const Key *> out;
    for (const Key &k : keys)
        out << &k;
    return out;
}

QString readText(const QString &path, qint64 max = 512 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(max));
}

// key -> value of a .properties file (comments, `key=value`, `key: value`, `key value`; no continuation lines).
QMap<QString, QString> parse(const QString &text)
{
    QMap<QString, QString> out;
    static const QRegularExpression re(QStringLiteral(R"(^\s*([^#!\s=:][^\s=:]*)\s*[=:\s]\s*(.*)$)"));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = re.match(line);
        if (m.hasMatch())
            out.insert(m.captured(1), m.captured(2).trimmed());
    }
    return out;
}

QString androidSdk()
{
    for (const char *var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty() && QFileInfo(v).isDir())
            return v;
    }
    const QString home = QDir::homePath() + QStringLiteral("/Android/Sdk");
    return QFileInfo(home).isDir() ? home : QString();
}

// Values found on this machine: value -> detail.
QList<QPair<QString, QString>> lookup(const QString &what, const QString &root)
{
    QList<QPair<QString, QString>> out;
    if (what == QLatin1String("@jdk")) {
        static QList<JdkScanner::Jdk> jdks;
        static QDateTime scanned;
        if (!scanned.isValid() || scanned.secsTo(QDateTime::currentDateTime()) > 30) { // once per completion burst
            jdks = JdkScanner::scan();
            scanned = QDateTime::currentDateTime();
        }
        for (const JdkScanner::Jdk &j : std::as_const(jdks))
            if (!j.jre)
                out << qMakePair(j.home, QStringLiteral("JDK ") + j.label());
    } else if (what == QLatin1String("@sdk")) {
        if (const QString sdk = androidSdk(); !sdk.isEmpty())
            out << qMakePair(sdk, QStringLiteral("Android SDK"));
    } else if (what == QLatin1String("@ndk") || what == QLatin1String("@cmake")) {
        QString sdk = parse(readText(root + QStringLiteral("/local.properties"))).value(QStringLiteral("sdk.dir"));
        if (sdk.isEmpty())
            sdk = androidSdk();
        if (!sdk.isEmpty()) {
            const QString sub = sdk + (what == QLatin1String("@ndk") ? QStringLiteral("/ndk") : QStringLiteral("/cmake"));
            const QStringList versions = QDir(sub).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
            for (const QString &v : versions)
                out << qMakePair(sub + QLatin1Char('/') + v, (what == QLatin1String("@ndk") ? QStringLiteral("NDK ") : QStringLiteral("CMake ")) + v);
        }
    } else if (what == QLatin1String("@dist")) {
        const QString current = JvmTools::wrapperVersion(root).value;
        if (!current.isEmpty())
            for (const char *type : {"bin", "all"})
                out << qMakePair(QStringLiteral("https\\://services.gradle.org/distributions/gradle-%1-%2.zip").arg(current, QLatin1String(type)),
                                 QStringLiteral("Gradle %1 (%2)").arg(current, QLatin1String(type)));
    }
    return out;
}

// Children of `parent` (one dotted segment) among `names`, starting with `partial`.
QList<Entry> childrenOf(const QStringList &names, const QHash<QString, QString> &details, const QString &parent, const QString &partial)
{
    QMap<QString, Entry> found;
    const QString prefix = parent.isEmpty() ? QString() : parent + QLatin1Char('.');
    for (const QString &name : names) {
        if (!name.startsWith(prefix))
            continue;
        const QString rest = name.mid(prefix.size());
        const int dot = rest.indexOf(QLatin1Char('.'));
        const QString segment = dot < 0 ? rest : rest.left(dot);
        if (!segment.startsWith(partial) || segment.size() == partial.size())
            continue;
        Entry &e = found[segment];
        e.label = segment.mid(partial.size());
        if (dot >= 0)
            e.group = true;
        else
            e.detail = details.value(name);
        if (e.group && e.detail.isEmpty())
            e.detail = QStringLiteral("…");
    }
    return found.values();
}

} // namespace

File kindOf(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    if (name == QLatin1String("gradle.properties"))
        return File::Gradle;
    if (name == QLatin1String("local.properties"))
        return File::Local;
    if (name == QLatin1String("gradle-wrapper.properties"))
        return File::Wrapper;
    return File::None;
}

QMap<QString, QString> load(const QString &root)
{
    QMap<QString, QString> out = parse(readText(root + QStringLiteral("/gradle.properties")));
    const QString user = qEnvironmentVariable("GRADLE_USER_HOME", QDir::homePath() + QStringLiteral("/.gradle"));
    const QMap<QString, QString> global = parse(readText(user + QStringLiteral("/gradle.properties")));
    for (auto it = global.cbegin(); it != global.cend(); ++it)
        out.insert(it.key(), it.value());
    return out;
}

QStringList usedKeys(const QString &root)
{
    static QHash<QString, QPair<qint64, QStringList>> cache; // root -> (newest build-file mtime, keys)
    QStringList files;
    QStringList dirs{root};
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!d.startsWith(QLatin1Char('.')) && d != QLatin1String("build") && d != QLatin1String("node_modules"))
            dirs << root + QLatin1Char('/') + d;
    qint64 stamp = 0;
    for (const QString &d : std::as_const(dirs))
        for (const char *f : {"build.gradle.kts", "build.gradle", "settings.gradle.kts", "settings.gradle"}) {
            const QFileInfo fi(d + QLatin1Char('/') + QLatin1String(f));
            if (fi.exists()) {
                files << fi.filePath();
                stamp = qMax(stamp, fi.lastModified().toMSecsSinceEpoch());
            }
        }
    const auto hit = cache.constFind(root);
    if (hit != cache.constEnd() && hit->first == stamp)
        return hit->second;
    static const QRegularExpression re(QStringLiteral(
        R"re((?:findProperty|hasProperty|getProperty|property|gradleProperty)\s*\(\s*["']([\w.\-]+)["']|extra\s*\[\s*"([\w.\-]+)"|\bext\s*\[\s*["']([\w.\-]+)["'])re"));
    QSet<QString> keys;
    for (const QString &f : std::as_const(files)) {
        QRegularExpressionMatchIterator it = re.globalMatch(readText(f));
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            keys.insert(!m.captured(1).isEmpty() ? m.captured(1) : !m.captured(2).isEmpty() ? m.captured(2) : m.captured(3));
        }
    }
    QStringList out(keys.begin(), keys.end());
    out.sort();
    cache.insert(root, {stamp, out});
    return out;
}

QList<Entry> complete(File file, const QString &beforeWord, const QString &root)
{
    QList<const Key *> keys = file == File::Gradle ? listOf(kGradleKeys) : file == File::Local ? listOf(kLocalKeys)
                            : file == File::Wrapper ? listOf(kWrapperKeys) : QList<const Key *>();
    if (keys.isEmpty())
        return {};
    static const QRegularExpression valueRe(QStringLiteral(R"(^\s*([\w.\-]+)\s*[=:]\s*(.*)$)"));
    static const QRegularExpression keyRe(QStringLiteral(R"(^\s*([\w.\-]*)$)"));

    if (const QRegularExpressionMatch m = valueRe.match(beforeWord); m.hasMatch()) {
        const QString key = m.captured(1);
        const QString tail = m.captured(2);
        QList<QPair<QString, QString>> values;
        for (const Key *k : std::as_const(keys))
            if (key == QLatin1String(k->name)) {
                const QString v = QString::fromUtf8(k->values);
                if (v.startsWith(QLatin1Char('@')))
                    values = lookup(v, root);
                else
                    for (const QString &x : v.split(QLatin1Char('|'), Qt::SkipEmptyParts))
                        values << qMakePair(x, QStringLiteral("value"));
            }
        // A comma-separated list (org.gradle.java.installations.paths) completes its last item.
        const QString item = key.endsWith(QLatin1String(".paths")) ? tail.mid(tail.lastIndexOf(QLatin1Char(',')) + 1) : tail;
        QList<Entry> out;
        for (const auto &v : std::as_const(values))
            if (v.first.startsWith(item) && v.first.size() > item.size())
                out << Entry{v.first.mid(item.size()), v.second, false, true};
        return out;
    }
    const QRegularExpressionMatch m = keyRe.match(beforeWord);
    if (!m.hasMatch())
        return {};
    QStringList names;
    QHash<QString, QString> details;
    for (const Key *k : std::as_const(keys)) {
        names << QString::fromLatin1(k->name);
        details.insert(names.last(), QString::fromUtf8(k->detail));
    }
    if (file == File::Gradle && !root.isEmpty()) {
        // Keys the build scripts read: offered first among their siblings, marked as such.
        for (const QString &k : usedKeys(root))
            if (!details.contains(k)) {
                names << k;
                details.insert(k, QStringLiteral("read by the build scripts"));
            }
    }
    const QString typed = m.captured(1);
    const int dot = typed.lastIndexOf(QLatin1Char('.'));
    return childrenOf(names, details, dot < 0 ? QString() : typed.left(dot), typed.mid(dot + 1));
}

QList<Entry> completeInScript(const QString &beforeWord, const QString &root)
{
    static const QRegularExpression re(QStringLiteral(
        R"re((?:findProperty|hasProperty|getProperty|property|gradleProperty)\s*\(\s*["']([\w.\-]*)$|(?:extra|ext)\s*\[\s*["']([\w.\-]*)$)re"));
    const QRegularExpressionMatch m = re.match(beforeWord);
    if (!m.hasMatch() || root.isEmpty())
        return {};
    const QString typed = m.captured(1) + m.captured(2);
    QList<Entry> out;
    const QMap<QString, QString> props = load(root);
    for (auto it = props.cbegin(); it != props.cend(); ++it)
        if (it.key().startsWith(typed) && it.key().size() > typed.size())
            out << Entry{it.key().mid(typed.size()), it.value().isEmpty() ? QStringLiteral("gradle.properties") : QStringLiteral("= ") + it.value().left(60), false, true};
    return out;
}

} // namespace GradleProperties
