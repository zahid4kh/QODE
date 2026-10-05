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

private:
    QList<Config> m_configs;
    QString m_kind;
    bool m_valid = false;
};
