#include "debug/DebugSession.h"

#include "debug/DebugWorker.h"

#include <QTcpServer>
#include <QThread>

DebugSession::DebugSession(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DebugSourceBreakpoints>();
    qRegisterMetaType<QList<DebugSourceBreakpoints>>();
    qRegisterMetaType<DebugVariable>();
    qRegisterMetaType<QList<DebugVariable>>();
    qRegisterMetaType<DebugStop>();
}

DebugSession::~DebugSession()
{
    if (m_worker) {
        DebugWorker *w = m_worker;
        QMetaObject::invokeMethod(w, [w] { w->stop(false); }, Qt::BlockingQueuedConnection);
    }
    cleanup();
}

int DebugSession::freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 5005;
    return probe.serverPort();
}

QString DebugSession::agentOption(int port)
{
    return QStringLiteral("-agentlib:jdwp=transport=dt_socket,server=y,suspend=y,address=127.0.0.1:%1").arg(port);
}

void DebugSession::start(int port, const QList<DebugSourceBreakpoints> &breakpoints, bool breakOnUncaught, int waitMs)
{
    if (m_worker) {
        DebugWorker *w = m_worker;
        QMetaObject::invokeMethod(w, [w] { w->stop(true); }, Qt::BlockingQueuedConnection);
        cleanup();
    }
    m_stop = {};
    m_thread = new QThread(this);
    auto *worker = new DebugWorker;
    worker->moveToThread(m_thread);
    m_worker = worker;
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    // A stopped session's last signals may still be queued: only the current one counts.
    const int gen = ++m_generation;
    connect(worker, &DebugWorker::attached, this, [this, gen](const QString &vm) {
        if (gen != m_generation)
            return;
        setState(State::Running);
        emit attached(vm);
    });
    connect(worker, &DebugWorker::stopped, this, [this, gen](const DebugStop &s) {
        if (gen != m_generation)
            return;
        m_stop = s;
        setState(State::Suspended);
        emit stopped(s);
    });
    connect(worker, &DebugWorker::resumed, this, [this, gen] {
        if (gen == m_generation)
            setState(State::Running);
    });
    connect(worker, &DebugWorker::variables, this, [this, gen](int token, const QList<DebugVariable> &vars) {
        if (gen == m_generation)
            emit variables(token, vars);
    });
    connect(worker, &DebugWorker::evaluated, this, [this, gen](int token, const DebugVariable &v, const QString &error) {
        if (gen == m_generation)
            emit evaluated(token, v, error);
    });
    connect(worker, &DebugWorker::breakpointStatus, this, [this, gen](const QString &path, int line, bool ok) {
        if (ok && gen == m_generation)
            emit breakpointVerified(path, line);
    });
    connect(worker, &DebugWorker::message, this, [this, gen](const QString &text) {
        if (gen == m_generation)
            emit message(text);
    });
    connect(worker, &DebugWorker::finished, this, [this, gen](const QString &reason) {
        if (gen != m_generation)
            return;
        cleanup();
        m_stop = {};
        setState(State::Idle);
        emit finished(reason);
    });
    m_thread->start();
    setState(State::Connecting);
    QMetaObject::invokeMethod(worker, [worker, port, waitMs, breakpoints, breakOnUncaught] { worker->start(port, waitMs, breakpoints, breakOnUncaught); });
}

void DebugSession::cleanup()
{
    if (!m_thread)
        return;
    if (m_worker)
        m_worker->disconnect(this);
    m_thread->quit();
    m_thread->wait(3000);
    m_thread->deleteLater();
    m_thread = nullptr;
    m_worker = nullptr;
}

void DebugSession::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged();
}

void DebugSession::setBreakpoints(const QList<DebugSourceBreakpoints> &breakpoints)
{
    if (DebugWorker *w = m_worker)
        QMetaObject::invokeMethod(w, [w, breakpoints] { w->setBreakpoints(breakpoints); });
}

void DebugSession::resume()
{
    if (DebugWorker *w = m_worker; w && m_state == State::Suspended)
        QMetaObject::invokeMethod(w, [w] { w->resume(); });
}

void DebugSession::step(int depth)
{
    if (DebugWorker *w = m_worker; w && m_state == State::Suspended)
        QMetaObject::invokeMethod(w, [w, depth] { w->step(depth); });
}

void DebugSession::pause()
{
    if (DebugWorker *w = m_worker; w && m_state == State::Running)
        QMetaObject::invokeMethod(w, [w] { w->pause(); });
}

void DebugSession::stop(bool terminate, const QString &reason)
{
    if (DebugWorker *w = m_worker)
        QMetaObject::invokeMethod(w, [w, terminate, reason] { w->stop(terminate, reason); });
}

void DebugSession::selectThread(quint64 thread)
{
    if (DebugWorker *w = m_worker)
        QMetaObject::invokeMethod(w, [w, thread] { w->selectThread(thread); });
}

int DebugSession::selectFrame(quint64 thread, int frameIndex)
{
    const int token = ++m_token;
    if (DebugWorker *w = m_worker)
        QMetaObject::invokeMethod(w, [w, token, thread, frameIndex] { w->selectFrame(token, thread, frameIndex); });
    return token;
}

int DebugSession::expand(quint64 objectId)
{
    const int token = ++m_token;
    if (DebugWorker *w = m_worker)
        QMetaObject::invokeMethod(w, [w, token, objectId] { w->expand(token, objectId); });
    return token;
}

int DebugSession::evaluate(quint64 thread, int frameIndex, const QString &expression)
{
    const int token = ++m_token;
    if (DebugWorker *w = m_worker)
        QMetaObject::invokeMethod(w, [w, token, thread, frameIndex, expression] { w->evaluate(token, thread, frameIndex, expression); });
    return token;
}
