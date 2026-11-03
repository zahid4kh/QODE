#pragma once

#include "debug/DebugTypes.h"

#include <QObject>
#include <QPointer>

class DebugWorker;
class QThread;

// One debug session at a time: the JVM runs in the terminal with a JDWP agent listening on a port QODE picked
// (suspend=y, so nothing runs before the breakpoints are in place), and the session attaches to it. The JDWP client
// (DebugWorker) runs on its own thread; this object is the window's side of it.
class DebugSession : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Connecting, Running, Suspended };

    explicit DebugSession(QObject *parent = nullptr);
    ~DebugSession() override;

    static int freePort();
    // The JVM option that makes a JVM wait for the debugger on `port`.
    static QString agentOption(int port);

    // Attaches to the JVM that is about to listen on `port` (it may still be building: waits up to `waitMs`).
    void start(int port, const QList<DebugSourceBreakpoints> &breakpoints, bool breakOnUncaught, int waitMs = 10 * 60 * 1000);
    State state() const { return m_state; }
    bool isActive() const { return m_state != State::Idle; }
    const DebugStop &lastStop() const { return m_stop; }

    void setBreakpoints(const QList<DebugSourceBreakpoints> &breakpoints);
    void resume();
    void stepOver() { step(1); }
    void stepInto() { step(0); }
    void stepOut() { step(2); }
    void pause();
    void stop(bool terminate = true, const QString &reason = QString());
    void selectThread(quint64 thread);
    // Each returns the token its answer (variables / evaluated) carries.
    int selectFrame(quint64 thread, int frameIndex);
    int expand(quint64 objectId);
    int evaluate(quint64 thread, int frameIndex, const QString &expression);

signals:
    void stateChanged();
    void attached(const QString &vm);
    void stopped(const DebugStop &stop);
    void variables(int token, const QList<DebugVariable> &vars);
    void evaluated(int token, const DebugVariable &result, const QString &error);
    void breakpointVerified(const QString &path, int line);
    void message(const QString &text);
    void finished(const QString &reason);

private:
    void step(int depth);
    void setState(State s);
    void cleanup();

    State m_state = State::Idle;
    QThread *m_thread = nullptr;
    QPointer<DebugWorker> m_worker;
    DebugStop m_stop;
    int m_token = 0;
    int m_generation = 0;
};
