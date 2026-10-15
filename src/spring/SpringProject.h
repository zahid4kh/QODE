#pragma once

#include <QString>

// Recognises a Spring Boot application (Gradle, either DSL and version-catalog aliases, or Maven) in the project root
// or one of its direct sub-folders, and builds the command that runs it. Qt Core only; nothing is run or written here.
struct SpringProject
{
    bool valid = false;
    bool maven = false;
    QString workDir;  // where the build tool is started: the folder with the wrapper / settings file / pom
    QString moduleDir; // the module that applies the Spring Boot plugin (== workDir for a single-module build)
    QString tool;     // "./gradlew", "gradle", "./mvnw" or "mvn"
    QString appName;  // spring.application.name, else the module folder name

    // User choices for one run (all optional); stored per project by SpringBar.
    struct Options
    {
        QString profiles;  // comma separated, becomes --spring.profiles.active
        int port = 0;      // 0 = whatever the application configures
        QString arguments; // extra application arguments
        QString command;   // complete replacement for the generated command
        QString jvmArgs;   // e.g. "-Xmx512m -Dfoo=bar", given to the application's JVM
        QString env;       // one KEY=VALUE per line, added to the environment of the run
    };

    static SpringProject detect(const QString &root);
    QString command() const { return command(Options()); }
    QString command(const Options &options) const;
    // Source folders that hold the application's classes (for resolving stack frames).
    QStringList sourceRoots() const;
    // Folder holding application*.properties / yml.
    QString resourcesDir() const;
    // The module's build file (build.gradle.kts, build.gradle or pom.xml).
    QString buildFile() const;
    // True when spring-boot-starter-actuator is among the module's dependencies.
    bool hasActuator() const;
    // Profiles that have a file application-<profile>.properties|yml|yaml.
    QStringList profiles() const;
    // "KEY=VALUE" lines of Options::env parsed (blank lines and # comments skipped).
    static QStringList parseEnv(const QString &text);
};
