#pragma once

#include <QList>
#include <QString>

// Recognises JVM projects (Java / Kotlin, built with Gradle in either DSL or with Maven), finds their `main`
// functions and proposes the commands that run, test and build them. Nothing is written into the project.
class JvmProject
{
public:
    struct Config {
        QString name;     // e.g. "Run Compose Desktop app"
        QString command;  // shell command, run from the project root
        bool runs = true; // starts the program (false: test / build helpers)
    };

    // Looks at `root` and, for multi-module builds, at its direct sub-folders; invalid when nothing runnable is found.
    static JvmProject detect(const QString &root);

    bool isValid() const { return m_valid; }
    // Runnable configurations first (best first), then test / build helpers.
    const QList<Config> &configs() const { return m_configs; }
    // The command of the best configuration.
    QString runCommand() const { return m_configs.isEmpty() ? QString() : m_configs.first().command; }
    // Human-readable summary, e.g. "Kotlin · Gradle (Kotlin DSL) · Compose Desktop".
    QString kind() const { return m_kind; }

    // The command that runs `mainClass` of the module holding `file`: Gradle through QODE's qodeRun init script (any
    // module with the java plugin, application plugin or not), Maven through exec:java. `initScripts` are added as
    // --init-script (debugging, task timings). Empty when `root` has no Gradle / Maven build or `file` is outside it.
    static QString mainCommand(const QString &root, const QString &file, const QString &mainClass, const QStringList &initScripts = {});
    // The command that runs the tests of `testClass` (only `method` when set) in the module holding `file`: Gradle
    // `cleanTest test --tests` (cleanTest so an up-to-date test task still runs; `initScripts` as above), Maven surefire's
    // -Dtest with `mavenDebugAgent` (a JDWP option) as maven.surefire.debug. Empty without a Gradle / Maven build.
    static QString testCommand(const QString &root, const QString &file, const QString &testClass, const QString &method,
                               const QStringList &initScripts = {}, const QString &mavenDebugAgent = {});
    // ~/.cache/QODE-AGENTIC/run/run-main.init.gradle (written when missing or outdated).
    static QString runMainInitScript();

private:
    QList<Config> m_configs;
    QString m_kind;
    bool m_valid = false;
};
