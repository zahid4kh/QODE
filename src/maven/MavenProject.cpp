#include "maven/MavenProject.h"
#include "platform/Platform.h"

#include "lsp/JarSource.h"
#include "editor/TextEncoding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <functional>

namespace MavenProject {

namespace {

QString readText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return TextEncoding::decode(f.readAll()).text;
}

// Reads one <project> element (the reader stands on its start tag). Only the elements the panel needs; nested
// <profiles>/<build> of profiles and <pluginManagement> are skipped.
Pom readProject(QXmlStreamReader &xml)
{
    Pom pom;
    QStringList path; // element names below <project>
    Plugin plugin;
    Dependency dep;
    Profile profile;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            path << xml.name().toString();
            const QString p = path.join(QLatin1Char('/'));
            if (p == QLatin1String("build/plugins/plugin"))
                plugin = Plugin();
            else if (p == QLatin1String("dependencies/dependency")) {
                dep = Dependency();
                dep.line = int(xml.lineNumber());
            } else if (p == QLatin1String("profiles/profile"))
                profile = Profile();
            continue;
        }
        if (token == QXmlStreamReader::EndElement) {
            if (path.isEmpty())
                return pom; // </project>
            const QString p = path.join(QLatin1Char('/'));
            if (p == QLatin1String("build/plugins/plugin")) {
                if (plugin.groupId.isEmpty())
                    plugin.groupId = QStringLiteral("org.apache.maven.plugins");
                pom.plugins << plugin;
            } else if (p == QLatin1String("dependencies/dependency")) {
                pom.dependencies << dep;
            } else if (p == QLatin1String("profiles/profile")) {
                if (!profile.id.isEmpty())
                    pom.profiles << profile;
            }
            path.removeLast();
            continue;
        }
        if (token != QXmlStreamReader::Characters || xml.isWhitespace())
            continue;
        const QString text = xml.text().toString().trimmed();
        const QString p = path.join(QLatin1Char('/'));
        if (p == QLatin1String("groupId"))
            pom.groupId = text;
        else if (p == QLatin1String("artifactId"))
            pom.artifactId = text;
        else if (p == QLatin1String("version"))
            pom.version = text;
        else if (p == QLatin1String("packaging"))
            pom.packaging = text;
        else if (p == QLatin1String("name"))
            pom.name = text;
        else if (p == QLatin1String("parent/groupId") && pom.groupId.isEmpty())
            pom.groupId = text;
        else if (p == QLatin1String("parent/version") && pom.version.isEmpty())
            pom.version = text;
        else if (p == QLatin1String("modules/module"))
            pom.modules << text;
        else if (p == QLatin1String("profiles/profile/id"))
            profile.id = text;
        else if (p == QLatin1String("profiles/profile/activation/activeByDefault"))
            profile.activeByDefault = text == QLatin1String("true");
        else if (p == QLatin1String("build/plugins/plugin/groupId"))
            plugin.groupId = text;
        else if (p == QLatin1String("build/plugins/plugin/artifactId"))
            plugin.artifactId = text;
        else if (p == QLatin1String("build/plugins/plugin/version"))
            plugin.version = text;
        else if (p == QLatin1String("dependencies/dependency/groupId"))
            dep.groupId = text;
        else if (p == QLatin1String("dependencies/dependency/artifactId"))
            dep.artifactId = text;
        else if (p == QLatin1String("dependencies/dependency/version"))
            dep.version = text;
        else if (p == QLatin1String("dependencies/dependency/scope"))
            dep.scope = text;
    }
    return pom;
}

QString jarOf(const Plugin &p, QString version)
{
    const QString base = localRepository() + QLatin1Char('/') + QString(p.groupId).replace(QLatin1Char('.'), QLatin1Char('/')) + QLatin1Char('/')
                         + p.artifactId;
    if (version.isEmpty() || version.contains(QLatin1Char('$'))) {
        // No version (inherited from a parent we did not read): the newest one downloaded.
        QStringList versions = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (versions.isEmpty())
            return {};
        version = versions.last();
    }
    return base + QLatin1Char('/') + version + QLatin1Char('/') + p.artifactId + QLatin1Char('-') + version + QStringLiteral(".jar");
}

} // namespace

QString Plugin::prefix() const
{
    static const QRegularExpression official(QStringLiteral(R"(^maven-(.+)-plugin$)"));
    static const QRegularExpression other(QStringLiteral(R"(^(.+)-maven-plugin$)"));
    if (const auto m = official.match(artifactId); m.hasMatch())
        return m.captured(1);
    if (const auto m = other.match(artifactId); m.hasMatch())
        return m.captured(1);
    return artifactId;
}

QString buildDir(const QString &root)
{
    if (root.isEmpty())
        return {};
    if (QFileInfo::exists(root + QStringLiteral("/pom.xml")))
        return root;
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (d.startsWith(QLatin1Char('.')) || d == QLatin1String("node_modules") || d == QLatin1String("target") || d == QLatin1String("build"))
            continue;
        if (QFileInfo::exists(root + QLatin1Char('/') + d + QStringLiteral("/pom.xml")))
            return root + QLatin1Char('/') + d;
    }
    return {};
}

QString runner(const QString &dir)
{
    const QString mvnw = QStringLiteral("mvnw");
    return Platform::wrapperFile(dir, mvnw).isEmpty() ? QStringLiteral("mvn") : Platform::wrapperCommand(dir, mvnw);
}

QString runnerFile(const QString &dir)
{
    const QString wrapper = Platform::wrapperFile(dir, QStringLiteral("mvnw"));
    return wrapper.isEmpty() ? QStringLiteral("mvn") : wrapper;
}

bool wrapperNeedsExecBit(const QString &dir)
{
    return Platform::wrapperNeedsExecBit(dir, QStringLiteral("mvnw"));
}

QString wrapperVersion(const QString &dir)
{
    static const QRegularExpression re(QStringLiteral(R"(apache-maven-([\d][\w.-]*?)-bin)"));
    return re.match(readText(dir + QStringLiteral("/.mvn/wrapper/maven-wrapper.properties"))).captured(1);
}

bool mavenAvailable(const QString &dir)
{
    return !Platform::wrapperFile(dir, QStringLiteral("mvnw")).isEmpty() || !QStandardPaths::findExecutable(QStringLiteral("mvn")).isEmpty();
}

Pom readPom(const QString &pomPath)
{
    QFile f(pomPath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QXmlStreamReader xml(&f);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("project")) {
            Pom pom = readProject(xml);
            pom.path = pomPath;
            if (pom.packaging.isEmpty())
                pom.packaging = QStringLiteral("jar");
            return pom;
        }
    }
    return {};
}

QList<Pom> readModules(const QString &dir, int depth)
{
    QList<Pom> out;
    std::function<void(const QString &, int)> walk = [&](const QString &d, int level) {
        const Pom pom = readPom(d + QStringLiteral("/pom.xml"));
        if (!pom.isValid())
            return;
        out << pom;
        if (level >= depth)
            return;
        for (const QString &m : pom.modules)
            walk(QDir::cleanPath(d + QLatin1Char('/') + m), level + 1);
    };
    walk(dir, 0);
    return out;
}

QMap<QString, Pom> parseEffectivePom(const QString &text)
{
    QMap<QString, Pom> out;
    QXmlStreamReader xml(text);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("project")) {
            const Pom pom = readProject(xml);
            if (pom.isValid())
                out.insert(pom.artifactId, pom);
        }
    }
    return out;
}

QList<Goal> pluginGoals(const Plugin &plugin)
{
    QList<Goal> out;
    const QString jar = jarOf(plugin, plugin.version);
    QByteArray data;
    if (jar.isEmpty() || !JarSource::readJarEntry(jar, QStringLiteral("META-INF/maven/plugin.xml"), &data))
        return out;
    QXmlStreamReader xml(data);
    QStringList path;
    Goal goal;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            path << xml.name().toString();
            if (path.join(QLatin1Char('/')) == QLatin1String("plugin/mojos/mojo"))
                goal = Goal();
        } else if (token == QXmlStreamReader::EndElement) {
            if (path.join(QLatin1Char('/')) == QLatin1String("plugin/mojos/mojo") && !goal.name.isEmpty())
                out << goal;
            if (!path.isEmpty())
                path.removeLast();
        } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            const QString p = path.join(QLatin1Char('/'));
            if (p == QLatin1String("plugin/mojos/mojo/goal"))
                goal.name = xml.text().toString().trimmed();
            else if (p == QLatin1String("plugin/mojos/mojo/description") && goal.description.isEmpty()) {
                // The first sentence, without the HTML Maven puts in.
                QString d = xml.text().toString();
                d.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
                d = d.simplified();
                const int dot = d.indexOf(QStringLiteral(". "));
                goal.description = dot > 0 ? d.left(dot + 1) : d;
            } else if (p == QLatin1String("plugin/mojos/mojo/phase"))
                goal.phase = xml.text().toString().trimmed();
        }
    }
    std::sort(out.begin(), out.end(), [](const Goal &a, const Goal &b) { return a.name < b.name; });
    return out;
}

QList<Goal> knownGoals(const QString &artifactId)
{
    static const QMap<QString, QStringList> known = {
        {QStringLiteral("maven-clean-plugin"), {QStringLiteral("clean")}},
        {QStringLiteral("maven-resources-plugin"), {QStringLiteral("resources"), QStringLiteral("testResources"), QStringLiteral("copy-resources")}},
        {QStringLiteral("maven-compiler-plugin"), {QStringLiteral("compile"), QStringLiteral("testCompile"), QStringLiteral("help")}},
        {QStringLiteral("maven-surefire-plugin"), {QStringLiteral("test"), QStringLiteral("help")}},
        {QStringLiteral("maven-failsafe-plugin"), {QStringLiteral("integration-test"), QStringLiteral("verify")}},
        {QStringLiteral("maven-jar-plugin"), {QStringLiteral("jar"), QStringLiteral("test-jar")}},
        {QStringLiteral("maven-war-plugin"), {QStringLiteral("war"), QStringLiteral("exploded"), QStringLiteral("inplace")}},
        {QStringLiteral("maven-install-plugin"), {QStringLiteral("install"), QStringLiteral("install-file")}},
        {QStringLiteral("maven-deploy-plugin"), {QStringLiteral("deploy"), QStringLiteral("deploy-file")}},
        {QStringLiteral("maven-site-plugin"), {QStringLiteral("site"), QStringLiteral("run"), QStringLiteral("stage"), QStringLiteral("deploy")}},
        {QStringLiteral("maven-dependency-plugin"), {QStringLiteral("tree"), QStringLiteral("analyze"), QStringLiteral("resolve"), QStringLiteral("copy-dependencies"), QStringLiteral("purge-local-repository")}},
        {QStringLiteral("maven-shade-plugin"), {QStringLiteral("shade")}},
        {QStringLiteral("maven-assembly-plugin"), {QStringLiteral("single")}},
        {QStringLiteral("maven-source-plugin"), {QStringLiteral("jar"), QStringLiteral("jar-no-fork"), QStringLiteral("test-jar")}},
        {QStringLiteral("maven-javadoc-plugin"), {QStringLiteral("javadoc"), QStringLiteral("jar"), QStringLiteral("aggregate")}},
        {QStringLiteral("spring-boot-maven-plugin"), {QStringLiteral("run"), QStringLiteral("repackage"), QStringLiteral("build-image"), QStringLiteral("build-info"), QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("test-run")}},
        {QStringLiteral("kotlin-maven-plugin"), {QStringLiteral("compile"), QStringLiteral("test-compile"), QStringLiteral("kapt")}},
        {QStringLiteral("exec-maven-plugin"), {QStringLiteral("java"), QStringLiteral("exec")}},
        {QStringLiteral("versions-maven-plugin"), {QStringLiteral("display-dependency-updates"), QStringLiteral("display-plugin-updates"), QStringLiteral("use-latest-releases"), QStringLiteral("set")}},
        {QStringLiteral("jacoco-maven-plugin"), {QStringLiteral("prepare-agent"), QStringLiteral("report"), QStringLiteral("check")}},
        {QStringLiteral("quarkus-maven-plugin"), {QStringLiteral("dev"), QStringLiteral("build"), QStringLiteral("test")}},
        {QStringLiteral("javafx-maven-plugin"), {QStringLiteral("run"), QStringLiteral("jlink")}},
    };
    QList<Goal> out;
    for (const QString &g : known.value(artifactId))
        out << Goal{g, QString(), QString()};
    return out;
}

QList<Plugin> defaultPlugins(const QString &packaging)
{
    const QString ap = QStringLiteral("org.apache.maven.plugins");
    QList<Plugin> out = {{ap, QStringLiteral("maven-clean-plugin"), {}}};
    if (packaging != QLatin1String("pom"))
        out << Plugin{ap, QStringLiteral("maven-resources-plugin"), {}} << Plugin{ap, QStringLiteral("maven-compiler-plugin"), {}}
            << Plugin{ap, QStringLiteral("maven-surefire-plugin"), {}}
            << Plugin{ap, packaging == QLatin1String("war") ? QStringLiteral("maven-war-plugin") : QStringLiteral("maven-jar-plugin"), {}};
    out << Plugin{ap, QStringLiteral("maven-install-plugin"), {}} << Plugin{ap, QStringLiteral("maven-deploy-plugin"), {}}
        << Plugin{ap, QStringLiteral("maven-site-plugin"), {}};
    return out;
}

QList<Phase> lifecycle()
{
    return {
        {QStringLiteral("clean"), QStringLiteral("Delete the target folder")},
        {QStringLiteral("validate"), QStringLiteral("Check that the project is correct and all information is available")},
        {QStringLiteral("compile"), QStringLiteral("Compile the main sources")},
        {QStringLiteral("test"), QStringLiteral("Run the unit tests")},
        {QStringLiteral("package"), QStringLiteral("Package the compiled code (jar, war …)")},
        {QStringLiteral("verify"), QStringLiteral("Run integration tests and checks on the package")},
        {QStringLiteral("install"), QStringLiteral("Install the package into the local repository (~/.m2)")},
        {QStringLiteral("site"), QStringLiteral("Generate the project's site documentation")},
        {QStringLiteral("deploy"), QStringLiteral("Copy the package to the remote repository")},
    };
}

QList<TreeModule> parseDependencyTree(const QString &output)
{
    QList<TreeModule> out;
    static const QRegularExpression header(QStringLiteral(R"(^\[INFO\] --- (?:maven-)?dependency(?:-plugin)?:[^:]+:tree .*@ (\S+) ---)"));
    // [INFO] |  +- group:artifact:type[:classifier]:version:scope (note)
    static const QRegularExpression node(QStringLiteral(R"(^\[INFO\] ((?:[| ]  )*)[+\\]- (\S+)(?:\s+\((.*)\))?\s*$)"));
    static const QRegularExpression root(QStringLiteral(R"(^\[INFO\] ([\w.\-]+:[\w.\-]+:\w+:\S+)\s*$)"));
    bool inTree = false;
    for (QString line : output.split(QLatin1Char('\n'))) {
        line.remove(QLatin1Char('\r'));
        if (const auto h = header.match(line); h.hasMatch()) {
            out << TreeModule{h.captured(1), QString(), {}};
            inTree = true;
            continue;
        }
        if (!inTree || out.isEmpty())
            continue;
        if (const auto n = node.match(line); n.hasMatch()) {
            TreeNode t;
            t.depth = int(n.captured(1).size() / 3);
            QString coord = n.captured(2);
            t.note = n.captured(3);
            if (coord.endsWith(QLatin1String("(optional)")))
                coord.chop(10);
            const QStringList parts = coord.split(QLatin1Char(':'));
            if (parts.size() < 4)
                continue;
            t.groupId = parts.at(0);
            t.artifactId = parts.at(1);
            t.type = parts.at(2);
            if (parts.size() >= 6) { // with a classifier
                t.classifier = parts.at(3);
                t.version = parts.at(4);
                t.scope = parts.at(5);
            } else {
                t.version = parts.at(3);
                t.scope = parts.size() > 4 ? parts.at(4) : QString();
            }
            t.optional = t.note.contains(QLatin1String("optional"));
            out.last().nodes << t;
            continue;
        }
        if (const auto r = root.match(line); r.hasMatch() && out.last().coordinate.isEmpty()) {
            out.last().coordinate = r.captured(1);
            continue;
        }
        if (!out.last().coordinate.isEmpty() && !line.startsWith(QLatin1String("[INFO] |")) && !line.startsWith(QLatin1String("[INFO] +"))
            && !line.startsWith(QLatin1String("[INFO] \\")))
            inTree = false; // the module's tree has ended
    }
    return out;
}

QString localRepository()
{
    static const QRegularExpression re(QStringLiteral(R"(<localRepository>\s*([^<]+?)\s*</localRepository>)"));
    const QString custom = re.match(readText(QDir::homePath() + QStringLiteral("/.m2/settings.xml"))).captured(1);
    if (!custom.isEmpty())
        return QString(custom).replace(QStringLiteral("${user.home}"), QDir::homePath());
    return QDir::homePath() + QStringLiteral("/.m2/repository");
}

} // namespace MavenProject
