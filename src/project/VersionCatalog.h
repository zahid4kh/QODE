#pragma once

#include <QList>
#include <QString>

// Reads a Gradle version catalog (gradle/libs.versions.toml) and answers "libs.<a>.<b>." completions in build scripts,
// the way Gradle generates the type-safe accessors: `kotlin-stdlib` becomes libs.kotlin.stdlib, and the catalog's
// [versions] / [bundles] / [plugins] sections live under libs.versions / libs.bundles / libs.plugins.
// Pure Qt Core: no GUI, no server.
class VersionCatalog
{
public:
    struct Entry {
        QString label;  // the next accessor segment, e.g. "stdlib"
        QString detail; // library coordinates + version, version number, plugin id ...; "…" for a group
        bool group = false; // more segments follow (libs.kotlin.<more>)
    };

    // `beforeWord` is the line text in front of the identifier being typed (it must end with the dot after `libs`, or
    // after a segment: "implementation(libs.kotlin."). The catalog is the gradle/libs.versions.toml found next to
    // `scriptPath` or in one of its parent folders. Empty when the text is not a catalog accessor or no catalog exists.
    static QList<Entry> complete(const QString &scriptPath, const QString &beforeWord);

    // True for the files whose `libs.` accessors the catalog serves: *.gradle and *.gradle.kts.
    static bool isBuildScript(const QString &path);
};
