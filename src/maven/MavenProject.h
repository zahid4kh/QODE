#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// What the Maven panel shows, read without running Maven where possible: the pom (modules, profiles, plugins,
// dependencies), the wrapper, the goals of a plugin (its META-INF/maven/plugin.xml in the local repository), plus the
// parsers for what Maven prints (help:effective-pom, dependency:tree). Qt Core only, nothing is run here.
namespace MavenProject {

struct Profile {
    QString id;
    bool activeByDefault = false;
};
struct Plugin {
    QString groupId, artifactId, version;
    QString prefix() const; // maven-compiler-plugin -> compiler, spring-boot-maven-plugin -> spring-boot
};
struct Dependency {
    QString groupId, artifactId, version, scope;
    int line = 0; // 1-based line in the pom
};
struct Pom {
    QString path;
    QString groupId, artifactId, version, packaging, name;
    QStringList modules;
    QList<Profile> profiles;
    QList<Plugin> plugins;  // <build><plugins> (not pluginManagement)
    QList<Dependency> dependencies;
    bool isValid() const { return !artifactId.isEmpty(); }
    QString title() const { return name.isEmpty() ? artifactId : name; }
};

// The folder holding the Maven build: `root` with a pom.xml, else its first direct sub-folder with one. "" = none.
QString buildDir(const QString &root);
QString runner(const QString &dir);              // ./mvnw (.\mvnw on Windows) when it can run, else mvn
QString runnerFile(const QString &dir);          // what to start for it: the wrapper file, else mvn
bool wrapperNeedsExecBit(const QString &dir);    // mvnw exists without its exec bit
QString wrapperVersion(const QString &dir);      // from .mvn/wrapper/maven-wrapper.properties, "" without a wrapper
bool mavenAvailable(const QString &dir);         // an executable mvnw, or mvn on PATH

Pom readPom(const QString &pomPath);
// The pom and those of its modules (recursively, at most `depth` levels), root first.
QList<Pom> readModules(const QString &dir, int depth = 3);
// help:effective-pom output: one <project> or several under <projects>; keyed by artifactId.
QMap<QString, Pom> parseEffectivePom(const QString &xml);

struct Goal {
    QString name, description, phase; // phase: the default binding, if any
};
// The plugin's goals from its jar in ~/.m2 (empty when it was never downloaded; `version` may be "").
QList<Goal> pluginGoals(const Plugin &plugin);
// Goals of well-known plugins, for those not downloaded yet.
QList<Goal> knownGoals(const QString &artifactId);
// Plugins every jar / war / pom project uses through the default lifecycle bindings.
QList<Plugin> defaultPlugins(const QString &packaging);

struct Phase {
    QString name, description;
};
QList<Phase> lifecycle(); // clean, validate, compile, test, package, verify, install, site, deploy

struct TreeNode {
    int depth = 0; // 0 = a direct dependency
    QString groupId, artifactId, type, classifier, version, scope;
    bool optional = false;
    QString note; // "omitted for duplicate" etc.
    QString coordinate() const { return groupId + QLatin1Char(':') + artifactId + QLatin1Char(':') + version; }
};
struct TreeModule {
    QString name;            // the module's artifactId
    QString coordinate;      // the module's own g:a:packaging:v line
    QList<TreeNode> nodes;
};
// `mvn -B dependency:tree` output ([INFO] lines).
QList<TreeModule> parseDependencyTree(const QString &output);

QString localRepository(); // ~/.m2/repository (or settings.xml's <localRepository>)

} // namespace MavenProject
