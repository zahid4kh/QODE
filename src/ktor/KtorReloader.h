#pragma once

#include "ktor/KtorProject.h"

#include <QObject>
#include <QProcess>
#include <QStringList>

class QTimer;

// The compile half of auto-reload: keeps the classes of a running Ktor server up to date. Gradle runs a continuous
// build (`gradlew -t classes`, the setup Ktor's documentation describes) that watches the sources itself, so an edit
// counts wherever it comes from (the editor, the QODE chat, git); Maven has no such mode, so `mvn compile` runs after
// each saved source (compileSoon()). Every compile that wrote new classes emits compiled(); KtorBar then lets Ktor
// reload in place or restarts the server. Runs in its own process group, like the server.
class KtorReloader : public QObject
{
    Q_OBJECT
public:
    enum class State { Off, Watching, Compiling, Failed };

    explicit KtorReloader(QObject *parent = nullptr);
    ~KtorReloader() override;

    void start(const KtorProject &project, const QStringList &extraEnv);
    void stop();
    void compileSoon(); // Maven: a source was saved (Gradle watches by itself)
    bool isOn() const { return m_on; }
    State state() const { return m_state; }
    // Compiler errors of the last failed compile ("Routing.kt:12: Unresolved reference 'x'.").
    QStringList errors() const { return m_errors; }
    // Why the continuous build ended on its own ("" while it runs or after stop()).
    QString failure() const { return m_failure; }

signals:
    void stateChanged();
    void compiled();                  // new classes are on disk
    void output(const QString &text); // compiler errors, for the log

private:
    void launch(const QStringList &args);
    void onOutput();
    void scan(const QString &line);
    void finishCycle(bool ok, bool wroteClasses);
    void setState(State s);
    void stopTree(); // ProcessTree::stop / kill on the running process
    void killTree();

    QProcess *m_proc;
    QTimer *m_killTimer, *m_debounce;
    KtorProject m_project;
    QStringList m_env, m_errors, m_cycleErrors;
    QString m_pending, m_failure;
    State m_state = State::Off;
    bool m_on = false, m_stopping = false, m_again = false;
    bool m_cycleFailed = false;
    int m_cycleExecuted = 0;
};
