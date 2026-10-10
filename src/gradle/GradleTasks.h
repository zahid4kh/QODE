#pragma once

#include <QList>
#include <QString>

// Reads the output of `gradle tasks`. Qt Core only, nothing is run here.
namespace GradleTasks {

struct Task {
    QString name;
    QString description;
};

struct Group {
    QString title; // "Build", "Signing", "Compose desktop" ... (the "tasks" suffix of Gradle's heading is dropped)
    QList<Task> tasks;
};

// The folder that holds the Gradle build: `root` itself, else its first direct sub-folder with a Gradle build file
// (the same places JvmProject looks). Empty when the project does not use Gradle.
QString buildDir(const QString &root);
// `./gradlew` when the folder has an executable wrapper, else `gradle`.
QString runner(const QString &dir);

// Parses the report printed by `gradle tasks` / `gradle tasks --all`.
QList<Group> parse(const QString &output);

} // namespace GradleTasks
