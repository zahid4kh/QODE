#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Text edits that add dependencies to a build file without disturbing the rest of it (Spring starters, Ktor plugins).
// Qt Core only; the caller reads and writes the file.
namespace BuildFileEdit {

// Adds `lines` (dependency declarations without indentation, e.g. `implementation("g:a")`) to the module's top-level
// `dependencies { }` block of build.gradle(.kts): after the last implementation / runtimeOnly / … line, so test
// dependencies stay last. A missing block is created. False when the block is not closed.
bool insertGradleLines(QString &text, const QStringList &lines);

struct MavenDependency
{
    QString group, artifact, version; // version may be empty (managed) or a property such as ${ktor_version}
    QString scope;                    // empty = compile
    bool optional = false;
};
// Adds the dependencies to the pom's <dependencies> (not the ones of <dependencyManagement> or <build>), in the file's
// own indentation. A missing section is created. False when the file has no </project>.
bool insertMavenDependencies(QString &text, const QList<MavenDependency> &deps);

// Offset of the '}' closing the block whose '{' is at `open`, or -1.
int matchBrace(const QString &s, int open);

} // namespace BuildFileEdit
