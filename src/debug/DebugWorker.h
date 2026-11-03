#pragma once

#include "debug/DebugTypes.h"
#include "debug/JdwpWire.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVector>

class QTcpSocket;

// The JDWP client of a debug session; lives on its own thread (DebugSession owns it). Requests from the window arrive
// as queued slot calls; inside a slot, commands are sent and their replies awaited (events that arrive meanwhile are
// queued and handled afterwards), so nothing here blocks the window. JVM events (breakpoint, step, exception, class
// prepare, VM death) come in on readyRead.
class DebugWorker : public QObject
{
    Q_OBJECT
public:
    explicit DebugWorker(QObject *parent = nullptr);
    ~DebugWorker() override;

public slots:
    // Connects to 127.0.0.1:port, trying again every 300 ms for `waitMs` (the JVM starts after a build).
    void start(int port, int waitMs, const QList<DebugSourceBreakpoints> &breakpoints, bool breakOnUncaught);
    void setBreakpoints(const QList<DebugSourceBreakpoints> &breakpoints);
    void resume();
    void step(int depth); // 0 into, 1 over, 2 out
    void pause();
    void stop(bool terminate, const QString &reason = QString()); // terminate: VirtualMachine.Exit; else detach
    void selectFrame(int token, quint64 thread, int frameIndex);
    void selectThread(quint64 thread);
    void expand(int token, quint64 objectId);
    void evaluate(int token, quint64 thread, int frameIndex, const QString &expression);

signals:
    void connecting(int attempt);
    void attached(const QString &vmName);
    void stopped(const DebugStop &stop);
    void resumed();
    void variables(int token, const QList<DebugVariable> &vars);
    void evaluated(int token, const DebugVariable &result, const QString &error);
    void breakpointStatus(const QString &path, int line, bool verified);
    void message(const QString &text);
    void finished(const QString &reason);

private:
    struct Location {
        char typeTag = 0;
        quint64 classId = 0, methodId = 0, index = 0;
    };
    struct Method {
        quint64 id = 0;
        QString name, signature;
        int modBits = 0;
    };
    struct LineEntry {
        quint64 index = 0;
        int line = 0;
    };
    struct Field {
        quint64 id = 0;
        QString name, signature;
        int modBits = 0;
    };
    struct SlotVar {
        quint64 codeIndex = 0;
        QString name, signature;
        int length = 0, slot = 0;
    };
    struct Value {
        char tag = 0;
        quint64 v = 0;
    };
    struct Installed {
        QString path;
        int line = 0;
        int requestId = 0;
    };

    void tryConnect();
    bool handshake();
    void setup();
    void fail(const QString &reason);
    void onReadyRead();
    bool takePacket(QByteArray *packet);
    void processEvents();
    void handleComposite(const QByteArray &body);
    bool command(int set, int cmd, const QByteArray &data, QByteArray *reply, int *errorCode = nullptr, int timeoutMs = 6000);
    void sendAsync(int set, int cmd, const QByteArray &data);

    // JVM data, cached per class / method.
    QString signature(quint64 type);
    QString sourceFile(quint64 type);
    QVector<Method> methods(quint64 type);
    QVector<LineEntry> lineTable(quint64 type, quint64 method);
    QVector<Field> fields(quint64 type);
    quint64 superclass(quint64 type);
    QVector<quint64> interfaces(quint64 type);
    bool isInstanceOf(quint64 type, const QString &signature);
    quint64 typeOf(quint64 object, char *typeTag = nullptr);
    QString stringValue(quint64 object);
    int lineOf(const Location &loc);
    QString methodName(quint64 type, quint64 method);

    Location readLocation(Jdwp::Reader &r) const;
    Value readTagged(Jdwp::Reader &r) const;
    Value readUntagged(Jdwp::Reader &r, char tag) const;
    QByteArray id(quint64 v, int size) const;

    void installFor(quint64 type, const QString &sig);
    void installAllLoaded();
    void requestClassPrepare(const QString &sourceName);
    void clearRequest(int kind, int requestId);
    void clearSteps();
    void reportStop(quint64 thread, const QString &reason, const QString &exception);
    QList<DebugFrame> frames(quint64 thread, QList<quint64> *ids);
    QList<DebugVariable> frameVariables(quint64 thread, quint64 frameId, const Location &loc);
    QList<DebugVariable> children(quint64 objectId);
    DebugVariable describe(const QString &name, const Value &v, const QString &declaredType);
    QString display(const Value &v, QString *type);
    bool invoke(quint64 object, quint64 thread, const QString &name, const QString &signature, Value *result);
    QList<QPair<quint64, QString>> threads();

    QTcpSocket *m_sock = nullptr;
    QByteArray m_buffer;
    QList<QByteArray> m_events;
    bool m_inCommand = false, m_processing = false, m_invoking = false;
    quint32 m_nextId = 0;
    int m_port = 0, m_attempt = 0, m_maxAttempts = 0;
    bool m_finished = false, m_attached = false, m_suspended = false;
    int m_fieldSize = 8, m_methodSize = 8, m_objectSize = 8, m_refSize = 8, m_frameSize = 8;
    bool m_breakOnUncaught = true;

    QList<DebugSourceBreakpoints> m_specs;
    QHash<QString, int> m_prepareRequests; // source name -> ClassPrepare request id
    QList<Installed> m_installed;           // breakpoint requests
    QSet<quint64> m_installedTypes;         // classes whose breakpoints are set (for the current specs)
    QList<int> m_stepRequests;
    quint64 m_stoppedThread = 0;
    QList<quint64> m_frameIds;              // of m_stoppedThread at the last stop
    QList<DebugFrame> m_frames;

    QHash<quint64, QString> m_sigCache, m_sourceCache;
    QHash<quint64, QVector<Method>> m_methodCache;
    QHash<QPair<quint64, quint64>, QVector<LineEntry>> m_lineCache;
    QHash<quint64, QVector<Field>> m_fieldCache;
    QHash<quint64, quint64> m_superCache;
    QHash<quint64, QVector<quint64>> m_ifaceCache;
    QHash<quint64, QSet<QString>> m_assignable;
    QHash<quint64, quint64> m_objectType;
    QHash<quint64, char> m_objectTypeTag;
};
