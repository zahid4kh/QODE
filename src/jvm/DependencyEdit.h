#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// Reading and writing dependency declarations for the dependency manager. Qt Core only; the caller writes the files.
namespace DependencyEdit {

// A dependency the build (or a .main.kts script) declares with a literal version.
struct Declared {
    QString group, artifact, version;
    QString file;          // where the version text is
    int start = 0;         // offset and length of the version text in that file
    int length = 0;
    QString where;         // "build.gradle.kts", "libs.versions.toml [versions] ktor" …
    QStringList sharedBy;  // a catalog version.ref / Maven property used by several artifacts: the others
};
// Build files of the root and its direct sub-folders, gradle/libs.versions.toml, pom.xml properties.
QList<Declared> declared(const QString &root);
QList<Declared> declaredInScript(const QString &path, const QString &text); // @file:DependsOn("g:a:v")

// The build files a dependency can be added to: root and direct sub-folders (build.gradle(.kts) / pom.xml).
QStringList buildFiles(const QString &root);
QStringList gradleScopes();
QStringList mavenScopes(); // "compile" means no <scope>

struct Change {
    bool ok = false;
    QString error;
    QString file, text;                     // the build file and its new text
    QList<QPair<QString, QString>> extra;   // other files to write (the version catalog)
    QString summary;                        // "implementation(libs.gson)"
};
// Adds group:artifact:version to the build file in `scope`. With `useCatalog` (and gradle/libs.versions.toml in the
// root) the library goes into the catalog and the build refers to it as libs.<alias>.
Change add(const QString &root, const QString &buildFile, const QString &group, const QString &artifact, const QString &version,
           const QString &scope, bool useCatalog);
bool buildUsesCatalog(const QString &buildFile); // its dependencies refer to libs.<alias>
QString catalogPath(const QString &root);         // "" when the project has none

// The script text with @file:DependsOn("coordinate") added after the other file annotations (or the shebang).
QString addDependsOn(const QString &text, const QString &coordinate);

// Replaces the version of each declaration (same file, any order) and returns the new texts by file.
QList<QPair<QString, QString>> applyUpdates(const QList<QPair<Declared, QString>> &updates);

} // namespace DependencyEdit
