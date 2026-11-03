#pragma once

#include "lsp/LspTypes.h"

#include <QHash>
#include <QObject>
#include <QSet>

class QProcess;

// Errors of Kotlin scripts without a language server: the Kotlin server only analyses Gradle / Maven projects, so a
// lone script.kts gets its diagnostics from kotlinc itself, compiling it without running it (about two seconds; the
// class files go to ~/.cache/QODE-AGENTIC/kts/). *.main.kts gets kotlin-main-kts.jar on the class path, so
// @file:DependsOn / @file:Repository resolve (their downloads are kotlinc's own Maven cache).
class ScriptCheck : public QObject
{
    Q_OBJECT
public:
    explicit ScriptCheck(QObject *parent = nullptr);
    ~ScriptCheck() override;

    static bool available(); // kotlinc found
    void check(const QString &path);
    void forget(const QString &path);
    QVector<LspDiagnostic> diagnostics(const QString &path) const { return m_results.value(path); }
    bool isChecking(const QString &path) const { return m_running.contains(path); }

    // "file:L:C: error: message" lines of kotlinc's output for `path` (other files' lines are skipped).
    static QVector<LspDiagnostic> parse(const QString &output, const QString &path);

signals:
    void started(const QString &path);
    void checked(const QString &path, int errors, int warnings);

private:
    void run(const QString &path);

    QHash<QString, QProcess *> m_running;
    QSet<QString> m_again; // saved again while being checked
    QHash<QString, QVector<LspDiagnostic>> m_results;
};
