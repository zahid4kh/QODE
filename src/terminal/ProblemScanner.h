#pragma once

#include <QSet>
#include <QString>
#include <QStringDecoder>
#include <QStringList>

#include <functional>

// An error or warning a compiler printed in the terminal, with the file it points at (existing on disk).
struct BuildProblem {
    QString path;
    int line = 0, column = 0; // 1-based; 0 when the output gives none (linker errors)
    bool error = true;        // else a warning
    QString message;
};

// Reads a shell's raw output line by line (escape sequences removed) and picks out compiler diagnostics:
// gcc / clang / javac / mypy "path:line[:col]: error|warning: message", rustc's "error: ..." followed by "--> path:line:col",
// ld's "file.cpp:(.text+0x1a): undefined reference to ...", "CMake Error at CMakeLists.txt:12 (...)", Kotlin's
// "e: file:///abs/X.kt:12:5 message" (Gradle, Maven) and Maven's "[ERROR] /abs/X.java:[12,5] message". Relative paths are
// looked up in make's "Entering directory" folder, then in the folders the terminal supplies.
class ProblemScanner
{
public:
    void setFolders(std::function<QStringList()> provider) { m_folders = std::move(provider); }
    // Feeds output; returns the problems completed by it (each one once until reset()).
    QList<BuildProblem> feed(const QByteArray &data);
    void reset(); // a new command starts

private:
    void scanLine(const QString &line, QList<BuildProblem> *out);
    QString resolve(const QString &raw) const;
    void add(BuildProblem p, QList<BuildProblem> *out);

    std::function<QStringList()> m_folders;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    QString m_partial;      // text after the last line break
    QString m_makeDir;      // make's current "Entering directory"
    QString m_rustMessage;  // "error[E0425]: ..." waiting for its "--> path:line:col"
    bool m_rustError = true;
    BuildProblem m_cmake;   // "CMake Error at ..." waiting for its message on a following line
    bool m_cmakePending = false;
    QSet<QString> m_seen;
};
