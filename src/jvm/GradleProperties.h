#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// gradle.properties, local.properties and gradle-wrapper.properties: completion of their keys (one dotted segment at a
// time, like the Spring config keys) and values (true / false, enums, installed JDKs, the Android SDK's NDKs …), the
// keys the project's build scripts read but nobody set yet, and the other way round: the defined properties inside
// findProperty("…") / providers.gradleProperty("…") in build scripts. Qt Core only.
namespace GradleProperties {

enum class File { None, Gradle, Local, Wrapper };
File kindOf(const QString &path);

struct Entry {
    QString label;  // what is inserted (the rest of the segment / value being typed)
    QString detail;
    bool group = false;
    bool value = false;
};

// `beforeWord` is the line text in front of the word being typed; `root` the project folder (for the keys its build
// scripts read and the SDK / JDK folders).
QList<Entry> complete(File file, const QString &beforeWord, const QString &root);

// Properties defined for the build: <root>/gradle.properties, then ~/.gradle/gradle.properties (which wins in Gradle).
// key -> value
QMap<QString, QString> load(const QString &root);
// Names the build scripts of `root` (and its direct sub-folders) read through findProperty / property / hasProperty /
// providers.gradleProperty / extra["…"].
QStringList usedKeys(const QString &root);
// In a build script: inside findProperty("…") and friends, the defined properties.
QList<Entry> completeInScript(const QString &beforeWord, const QString &root);

} // namespace GradleProperties
