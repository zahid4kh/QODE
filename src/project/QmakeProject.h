#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// A best-effort reader for qmake .pro files. It understands plain assignments (=, +=, -=, *=), $$VAR
// expansion, include(), SUBDIRS and the common platform / Qt-version conditions, and ignores the rest. It is
// used to give the language server the include paths and defines of a qmake project that has no
// compile_commands.json, and to suggest a command that builds and runs the project.
class QmakeProject
{
public:
    // Looks for a .pro file in `root` (not below it); an invalid project when there is none.
    static QmakeProject detect(const QString &root);

    bool isValid() const { return !m_proFile.isEmpty(); }
    QString proFile() const { return m_proFile; }
    QString proFileName() const;
    bool isApp() const { return !m_runPath.isEmpty(); }

    // Shell command (with {project} placeholder) that configures, builds in build/ and runs the application.
    QString runCommand() const;
    // Compiler flags for the language server: -std, -I, -D, Qt module headers, pkg-config packages.
    QStringList compilerFlags() const;

private:
    struct Part {
        QString file, dir;
        QString target, destDir;
        bool app = true;
        QStringList includes, defines, config, qt, cxxFlags, pkgConfig;
    };

    QString m_root, m_proFile;
    QString m_runPath; // executable relative to the build folder; empty when the project builds no application
    QVector<Part> m_parts;
};
