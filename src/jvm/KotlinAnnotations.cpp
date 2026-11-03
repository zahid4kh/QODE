#include "jvm/KotlinAnnotations.h"

#include "jvm/MavenCentral.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>

namespace KotlinAnnotations {

namespace {

struct Target {
    const char *name, *insert, *detail;
    bool scriptOnly;
};

const Target kTargets[] = {
    {"DependsOn", "DependsOn(\"$1\")$0", "a library: group:artifact:version", true},
    {"Repository", "Repository(\"$1\")$0", "a Maven repository URL", true},
    {"Import", "Import(\"$1\")$0", "another script to include", true},
    {"CompilerOptions", "CompilerOptions(\"$1\")$0", "kotlinc options", true},
    {"JvmName", "JvmName(\"$1\")$0", "JVM class name of the file", false},
    {"JvmMultifileClass", "JvmMultifileClass", "one class for several files", false},
    {"OptIn", "OptIn($1::class)$0", "opt in for the file", false},
    {"Suppress", "Suppress(\"$1\")$0", "suppress warnings in the file", false},
};

const Target kAnnotations[] = {
    {"file:", "file:", "annotate the whole file", false},
    {"JvmStatic", "JvmStatic", "static member for Java", false},
    {"JvmField", "JvmField", "public field, no getter", false},
    {"JvmOverloads", "JvmOverloads", "overloads for default arguments", false},
    {"JvmName", "JvmName(\"$1\")$0", "JVM name", false},
    {"JvmInline", "JvmInline", "inline value class", false},
    {"JvmRecord", "JvmRecord", "compile to a Java record", false},
    {"Throws", "Throws($1::class)$0", "checked exceptions for Java", false},
    {"Volatile", "Volatile", "volatile backing field", false},
    {"Synchronized", "Synchronized", "synchronized method", false},
    {"Transient", "Transient", "transient backing field", false},
    {"Deprecated", "Deprecated(\"$1\")$0", "deprecated", false},
    {"Suppress", "Suppress(\"$1\")$0", "suppress warnings", false},
    {"OptIn", "OptIn($1::class)$0", "opt in", false},
    {"PublishedApi", "PublishedApi", "internal, used by public inline", false},
    {"DslMarker", "DslMarker", "DSL marker", false},
    {"Serializable", "Serializable", "kotlinx.serialization", false},
};

const char *const kRepositories[] = {
    "https://repo.maven.apache.org/maven2/",
    "https://maven.google.com/",
    "https://jitpack.io",
    "https://central.sonatype.com/repository/maven-snapshots/",
    "https://maven.pkg.jetbrains.space/public/p/compose/dev",
};

const char *const kOptIns[] = {
    "ExperimentalStdlibApi", "ExperimentalCoroutinesApi", "DelicateCoroutinesApi", "FlowPreview", "ExperimentalSerializationApi",
    "ExperimentalTime", "ExperimentalUuidApi", "ExperimentalContracts", "ExperimentalUnsignedTypes", "ExperimentalPathApi",
    "ExperimentalEncodingApi", "InternalCoroutinesApi",
};

struct Local {
    QMap<QString, QStringList> versions; // group:artifact -> versions, newest first
    QStringList coordinates;             // group:artifact:newest
};

void addVersion(QMap<QString, QStringList> &map, const QString &ga, const QString &version)
{
    QStringList &list = map[ga];
    if (!list.contains(version))
        list << version;
}

const Local &local()
{
    static Local cache;
    static bool loaded = false;
    if (loaded)
        return cache;
    loaded = true;
    QElapsedTimer timer;
    timer.start();
    // ~/.m2/repository/<group path>/<artifact>/<version>/<artifact>-<version>.pom
    const QString m2 = QDir::homePath() + QStringLiteral("/.m2/repository");
    QDirIterator it(m2, {QStringLiteral("*.pom")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && timer.elapsed() < 1500) {
        const QString path = it.next();
        const QString rel = path.mid(m2.size() + 1);
        QStringList parts = rel.split(QLatin1Char('/'));
        if (parts.size() < 4)
            continue;
        const QString version = parts.at(parts.size() - 2);
        const QString artifact = parts.at(parts.size() - 3);
        if (!parts.last().startsWith(artifact + QLatin1Char('-') + version))
            continue;
        const QString group = parts.mid(0, parts.size() - 3).join(QLatin1Char('.'));
        addVersion(cache.versions, group + QLatin1Char(':') + artifact, version);
    }
    // ~/.gradle/caches/modules-2/files-2.1/<group>/<artifact>/<version>/<hash>/…
    const QString gradle = qEnvironmentVariable("GRADLE_USER_HOME", QDir::homePath() + QStringLiteral("/.gradle"))
                           + QStringLiteral("/caches/modules-2/files-2.1");
    const QDir gdir(gradle);
    for (const QString &group : gdir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir g(gdir.filePath(group));
        for (const QString &artifact : g.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            for (const QString &version : QDir(g.filePath(artifact)).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                addVersion(cache.versions, group + QLatin1Char(':') + artifact, version);
    }
    for (auto i = cache.versions.begin(); i != cache.versions.end(); ++i) {
        std::sort(i->begin(), i->end(), [](const QString &a, const QString &b) { return MavenCentral::compareVersions(a, b) > 0; });
        cache.coordinates << i.key() + QLatin1Char(':') + MavenCentral::newestFor(*i);
    }
    return cache;
}

} // namespace

Where contextOf(const QString &beforeWord)
{
    Where w;
    static const QRegularExpression stringArg(QStringLiteral(R"re(@file:\s*(DependsOn|Repository|Import)\s*\((?:[^)]*,)?\s*"([^"]*)$)re"));
    if (const QRegularExpressionMatch m = stringArg.match(beforeWord); m.hasMatch()) {
        const QString kind = m.captured(1);
        w.context = kind == QLatin1String("DependsOn") ? Context::DependsOn : kind == QLatin1String("Repository") ? Context::Repository : Context::Import;
        w.typed = m.captured(2);
        w.argStart = int(m.capturedStart(2));
        return w;
    }
    static const QRegularExpression optIn(QStringLiteral(R"(@(?:file:)?OptIn\s*\(\s*(?:[\w.]+::class\s*,\s*)*$)"));
    if (optIn.match(beforeWord).hasMatch()) {
        w.context = Context::OptIn;
        return w;
    }
    static const QRegularExpression target(QStringLiteral(R"(^\s*@file:\s*$)"));
    if (target.match(beforeWord).hasMatch()) {
        w.context = Context::FileTarget;
        return w;
    }
    static const QRegularExpression at(QStringLiteral(R"((?:^|[\s(,])@$)"));
    if (at.match(beforeWord).hasMatch())
        w.context = Context::At;
    return w;
}

QList<Entry> complete(const QString &path, const Where &where, bool served)
{
    QList<Entry> out;
    const bool script = path.endsWith(QLatin1String(".kts")) && !path.endsWith(QLatin1String(".gradle.kts"));
    switch (where.context) {
    case Context::None:
        break;
    case Context::FileTarget:
        for (const Target &t : kTargets)
            if (script || !t.scriptOnly)
                out << Entry{QString::fromLatin1(t.name), QString::fromUtf8(t.detail), QString::fromUtf8(t.insert), true, 7, false};
        break;
    case Context::At:
        for (const Target &t : kAnnotations) {
            const bool fileTarget = qstrcmp(t.name, "file:") == 0;
            if (served && !fileTarget) // the server lists annotations itself
                continue;
            out << Entry{QString::fromLatin1(t.name), QString::fromUtf8(t.detail), QString::fromUtf8(t.insert), true, fileTarget ? 14 : 7, false};
        }
        break;
    case Context::OptIn:
        for (const char *o : kOptIns)
            out << Entry{QString::fromLatin1(o), QStringLiteral("opt-in marker"), QString::fromLatin1(o) + QStringLiteral("::class"), false, 7, false};
        break;
    case Context::Repository:
        for (const char *r : kRepositories)
            if (QString::fromLatin1(r).startsWith(where.typed, Qt::CaseInsensitive) || where.typed.isEmpty())
                out << Entry{QString::fromLatin1(r), QStringLiteral("repository"), QString(), false, 12, true};
        break;
    case Context::Import: {
        const QFileInfo fi(path);
        for (const QString &name : fi.dir().entryList({QStringLiteral("*.kts")}, QDir::Files, QDir::Name))
            if (name != fi.fileName() && !name.endsWith(QLatin1String(".gradle.kts")))
                out << Entry{name, QStringLiteral("script"), QString(), false, 17, true};
        break;
    }
    case Context::DependsOn: {
        // group:artifact: typed in full: its versions; else the artifacts that match what is typed.
        const QStringList parts = where.typed.split(QLatin1Char(':'));
        if (parts.size() >= 3) {
            for (const QString &v : localVersions(parts.at(0), parts.at(1)))
                out << Entry{v, QStringLiteral("downloaded"), parts.at(0) + QLatin1Char(':') + parts.at(1) + QLatin1Char(':') + v, false, 12, true};
            break;
        }
        const QString needle = where.typed.toLower();
        for (const QString &c : localArtifacts())
            if (needle.isEmpty() || c.toLower().contains(needle))
                out << Entry{c, QStringLiteral("downloaded"), QString(), false, 9, true};
        break;
    }
    }
    return out;
}

QStringList localArtifacts() { return local().coordinates; }

QStringList localVersions(const QString &group, const QString &artifact)
{
    return local().versions.value(group + QLatin1Char(':') + artifact);
}

} // namespace KotlinAnnotations
