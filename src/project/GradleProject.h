#pragma once

#include <QString>

// Recognises Gradle projects that build a runnable desktop application (Compose Desktop, or any project with
// the `application` plugin) and suggests the command that runs them.
class GradleProject
{
public:
    // Looks at `root` and, for multi-module builds, at its direct sub-folders; invalid when nothing runnable is found.
    static GradleProject detect(const QString &root);

    bool isValid() const { return !m_command.isEmpty(); }
    bool isCompose() const { return m_compose; }
    // "./gradlew run" (or ":module:run" for a module, "gradle run" without a wrapper).
    QString runCommand() const { return m_command; }
    // Human-readable kind, e.g. "Compose Desktop".
    QString kind() const { return m_compose ? QStringLiteral("Compose Desktop") : QStringLiteral("Gradle application"); }
    QString module() const { return m_module; }

    // Settings key under which a project-wide Gradle run command is stored.
    static bool isGradleFileKey(const QString &key);

private:
    QString m_command, m_module;
    bool m_compose = false;
};
