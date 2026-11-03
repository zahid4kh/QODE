#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Recognises a Ktor server (Gradle with either DSL, the Ktor plugin, Ktor's version catalog or plain coordinates, or
// Maven) in the project root or one of its direct sub-folders, and builds the command that runs it. A Ktor *client*
// alone does not count. Only build files are read, so detection stays cheap enough to repeat on every build-file edit.
// Qt Core only; nothing is run or written here.
struct KtorProject
{
    bool valid = false;
    bool maven = false;
    QString workDir;   // where the build tool is started: the folder with the wrapper / settings file / pom
    QString moduleDir; // the module that holds the server (== workDir for a single-module build)
    QString tool;      // "./gradlew", "gradle", "./mvnw" or "mvn"
    QString appName;   // the module folder name
    QString engine;    // Netty, CIO, Jetty, Tomcat or "" when the build names none
    QString mainClass; // from the build file ("io.ktor.server.netty.EngineMain", "com.example.ApplicationKt" …)
    QString version;   // Ktor version when the build or its catalog spells it out, else ""

    // User choices for one run (all optional); stored per project by KtorBar.
    struct Options
    {
        int port = 0;             // 0 = what the application configures (passed as -port=N, which EngineMain reads)
        bool development = false; // -Dio.ktor.development=true: development mode (more logging, detailed errors)
        bool autoReload = false;  // recompile on change and reload (implies development mode), see KtorReloader
        bool liveRoutes = true;   // open a local debug port so the panel reads the routing tree of the running server
        QString arguments;        // extra application arguments ("-P:ktor.deployment.host=127.0.0.1")
        QString command;          // complete replacement for the generated command
        QString jvmArgs;          // added to the application's JVM options
        QString env;              // one KEY=VALUE per line, added to the environment of the run
    };

    static KtorProject detect(const QString &root);
    // The command that runs the server. Gradle gets the application's JVM options through an init script outside the
    // project (runScriptPath(), written by KtorBar before each start from runScript()): they reach only the JVM that
    // `run` forks (JAVA_TOOL_OPTIONS would reach Gradle's own JVMs too) and they come after the build's
    // applicationDefaultJvmArgs, so they win. Maven's exec:java runs the server inside Maven's JVM: JAVA_TOOL_OPTIONS.
    QString command(const Options &options, int debugPort = 0) const;
    QStringList jvmOptions(const Options &options, int debugPort) const; // what the application's JVM gets
    QString runScriptPath() const;                                       // ~/.cache/QODE-AGENTIC/ktor/run-<hash>.gradle
    QString runScript(const Options &options, int debugPort) const;
    // Where the build puts the compiled classes / resources: what Ktor watches for auto-reload (-watch=…).
    QStringList outputDirs() const;
    QString gradlePath() const; // ":" or ":server": the Gradle project of the module
    QStringList sourceRoots() const; // src/main/kotlin … (+ the test roots)
    QString resourcesDir() const;
    QString buildFile() const;
    // application.yaml / .yml / .conf in the resources, the first found, or "".
    QString configFile() const;
    // The text of the build file plus the version catalog next to it (or in a parent folder): what dependency checks read.
    QString buildText() const;
    // True when the build already declares the artifact ("ktor-server-cors"), as coordinates, through the version catalog
    // or as an accessor of Ktor's catalog (`accessor` = "server.cors" for ktorLibs.server.cors).
    bool hasArtifact(const QString &artifact, const QString &accessor = QString()) const;
    // ktorLibs.server.cors style accessors are available (settings.gradle(.kts) imports Ktor's version catalog).
    bool usesKtorCatalog() const;

    // One value of the configuration file, flattened to a dotted key ("ktor.deployment.port").
    struct ConfigEntry
    {
        QString key, value;
        int line = 0; // 1-based
    };
    static QVector<ConfigEntry> readConfig(const QString &path);
    // The port a configured value means: "8080", "$PORT:8080" / "${PORT:8080}" (YAML), "${?PORT}" after a literal
    // (HOCON keeps the earlier value). 0 when it cannot be told.
    static int portOf(const QString &value);
};
