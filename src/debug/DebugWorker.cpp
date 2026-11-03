#include "debug/DebugWorker.h"

#include <QDeadlineTimer>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

using Jdwp::Reader;
using Jdwp::put;
using Jdwp::putString;

namespace {

// Command sets / commands (JDWP spec).
enum : int {
    VirtualMachine = 1, VmVersion = 1, AllClasses = 3, AllThreads = 4, Dispose = 6, IDSizes = 7, VmSuspend = 8, VmResume = 9, VmExit = 10,
    ReferenceType = 2, RtSignature = 1, RtFields = 4, RtMethods = 5, RtSourceFile = 7, RtInterfaces = 10,
    ClassType = 3, Superclass = 1,
    MethodSet = 6, LineTable = 1, VariableTable = 2,
    ObjectReference = 9, OrReferenceType = 1, OrGetValues = 2, InvokeMethod = 6,
    StringReference = 10, StringValue = 1,
    ThreadReference = 11, ThreadName = 1, ThreadResume = 3, ThreadFrames = 6,
    ArrayReference = 13, ArrayLength = 1, ArrayGetValues = 2,
    EventRequest = 15, ErSet = 1, ErClear = 2,
    StackFrame = 16, SfGetValues = 1, SfThisObject = 3,
};
// Event kinds.
enum : int { SingleStep = 1, Breakpoint = 2, Exception = 4, ThreadStart = 6, ThreadDeath = 7, ClassPrepare = 8, ClassUnload = 9, VmStart = 90, VmDeath = 99 };
constexpr int kAbsentInformation = 101;
constexpr int kMaxFrames = 80;
constexpr int kMaxChildren = 200;
constexpr int kMaxInvokesPerRequest = 80;

QString primitive(char tag, quint64 v)
{
    switch (tag) {
    case 'Z': return v ? QStringLiteral("true") : QStringLiteral("false");
    case 'B': return QString::number(qint8(v));
    case 'S': return QString::number(qint16(v));
    case 'I': return QString::number(qint32(v));
    case 'J': return QString::number(qint64(v));
    case 'C': {
        const QChar c{char16_t(v)};
        return c.isPrint() ? QStringLiteral("'%1'").arg(c) : QStringLiteral("'\\u%1'").arg(int(v), 4, 16, QLatin1Char('0'));
    }
    case 'F': {
        const quint32 bits = quint32(v);
        float f;
        memcpy(&f, &bits, 4);
        return QString::number(double(f), 'g', 8);
    }
    case 'D': {
        double d;
        memcpy(&d, &v, 8);
        return QString::number(d, 'g', 15);
    }
    case 'V': return QStringLiteral("void");
    default: return {};
    }
}

QString quoted(const QString &s)
{
    QString t = s.left(400);
    t.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\"")).replace(QLatin1Char('\n'), QStringLiteral("\\n"))
        .replace(QLatin1Char('\t'), QStringLiteral("\\t")).replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QLatin1Char('"') + t + (s.size() > 400 ? QStringLiteral("…\"") : QStringLiteral("\""));
}

const QStringList &stepExcludes()
{
    static const QStringList list = {QStringLiteral("java.*"), QStringLiteral("javax.*"), QStringLiteral("jdk.*"), QStringLiteral("sun.*"),
                                     QStringLiteral("com.sun.*"), QStringLiteral("kotlin.*"), QStringLiteral("kotlinx.*"), QStringLiteral("groovy.*"),
                                     QStringLiteral("org.gradle.*"), QStringLiteral("org.codehaus.*"), QStringLiteral("org.jetbrains.kotlin.*")};
    return list;
}

} // namespace

DebugWorker::DebugWorker(QObject *parent)
    : QObject(parent)
{
}

DebugWorker::~DebugWorker()
{
    delete m_sock;
}

QByteArray DebugWorker::id(quint64 v, int size) const
{
    QByteArray b;
    put(b, v, size);
    return b;
}

// --- connection ---------------------------------------------------------------------------------------------------------

void DebugWorker::start(int port, int waitMs, const QList<DebugSourceBreakpoints> &breakpoints, bool breakOnUncaught)
{
    m_port = port;
    m_specs = breakpoints;
    m_breakOnUncaught = breakOnUncaught;
    m_maxAttempts = qMax(1, waitMs / 300);
    m_attempt = 0;
    m_sock = new QTcpSocket;
    tryConnect();
}

void DebugWorker::tryConnect()
{
    if (m_finished)
        return;
    emit connecting(++m_attempt);
    m_sock->abort();
    m_sock->connectToHost(QStringLiteral("127.0.0.1"), quint16(m_port));
    if (m_sock->waitForConnected(250) && handshake()) {
        connect(m_sock, &QTcpSocket::readyRead, this, &DebugWorker::onReadyRead);
        connect(m_sock, &QTcpSocket::disconnected, this, [this] { fail(tr("The program ended.")); });
        setup();
        return;
    }
    if (m_attempt >= m_maxAttempts) {
        fail(tr("Could not attach to the program's debug port %1.").arg(m_port));
        return;
    }
    QTimer::singleShot(300, this, &DebugWorker::tryConnect);
}

bool DebugWorker::handshake()
{
    const QByteArray hello("JDWP-Handshake");
    m_sock->write(hello);
    QByteArray got;
    QDeadlineTimer deadline(3000);
    while (got.size() < hello.size()) {
        if (m_sock->bytesAvailable() == 0 && !m_sock->waitForReadyRead(int(qMax<qint64>(1, deadline.remainingTime()))))
            return false;
        got += m_sock->read(hello.size() - got.size());
    }
    if (got != hello)
        return false;
    QByteArray r;
    if (!command(VirtualMachine, IDSizes, {}, &r))
        return false;
    Reader rd{r};
    m_fieldSize = rd.i32();
    m_methodSize = rd.i32();
    m_objectSize = rd.i32();
    m_refSize = rd.i32();
    m_frameSize = rd.i32();
    return !rd.bad;
}

void DebugWorker::setup()
{
    m_attached = true;
    QByteArray r;
    QString vm;
    if (command(VirtualMachine, VmVersion, {}, &r)) {
        Reader rd{r};
        rd.str();
        rd.i32();
        rd.i32();
        const QString version = rd.str();
        vm = rd.str() + QStringLiteral(" ") + version;
    }
    emit attached(vm.trimmed());
    if (m_breakOnUncaught) {
        QByteArray q;
        q.append(char(Exception));
        q.append(char(2)); // suspend all
        put(q, 1, 4);
        q.append(char(8)); // ExceptionOnly
        put(q, 0, m_refSize);
        q.append(char(0)); // caught
        q.append(char(1)); // uncaught
        command(EventRequest, ErSet, q, &r);
    }
    setBreakpoints(m_specs);
    processEvents(); // VMStart (suspend=y): resumes once everything is set
}

void DebugWorker::fail(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->abort();
    }
    emit finished(reason);
}

void DebugWorker::stop(bool terminate, const QString &reason)
{
    if (m_finished)
        return;
    if (m_attached && m_sock && m_sock->state() == QAbstractSocket::ConnectedState) {
        QByteArray r;
        if (terminate) {
            QByteArray q;
            put(q, 1, 4);
            command(VirtualMachine, VmExit, q, &r, nullptr, 2000);
        } else {
            clearSteps();
            for (const Installed &i : std::as_const(m_installed))
                clearRequest(Breakpoint, i.requestId);
            command(VirtualMachine, Dispose, {}, &r, nullptr, 2000);
        }
    }
    fail(!reason.isEmpty() ? reason : terminate ? tr("Stopped.") : tr("Detached; the program keeps running."));
}

// --- packets ------------------------------------------------------------------------------------------------------------

bool DebugWorker::takePacket(QByteArray *packet)
{
    if (m_buffer.size() < 11)
        return false;
    Reader h{m_buffer};
    const quint32 len = quint32(h.num(4));
    if (len < 11 || len > 64u * 1024 * 1024) {
        m_buffer.clear();
        return false;
    }
    if (quint32(m_buffer.size()) < len)
        return false;
    *packet = m_buffer.left(int(len));
    m_buffer.remove(0, int(len));
    return true;
}

void DebugWorker::sendAsync(int set, int cmd, const QByteArray &data)
{
    QByteArray packet;
    put(packet, quint64(11 + data.size()), 4);
    put(packet, ++m_nextId, 4);
    packet.append(char(0));
    packet.append(char(set));
    packet.append(char(cmd));
    packet.append(data);
    m_sock->write(packet);
}

bool DebugWorker::command(int set, int cmd, const QByteArray &data, QByteArray *reply, int *errorCode, int timeoutMs)
{
    if (!m_sock || m_sock->state() != QAbstractSocket::ConnectedState)
        return false;
    const quint32 cid = ++m_nextId;
    QByteArray packet;
    put(packet, quint64(11 + data.size()), 4);
    put(packet, cid, 4);
    packet.append(char(0));
    packet.append(char(set));
    packet.append(char(cmd));
    packet.append(data);
    m_sock->write(packet);
    const bool outer = !m_inCommand;
    m_inCommand = true;
    QDeadlineTimer deadline(timeoutMs);
    bool ok = false, done = false;
    while (!done) {
        QByteArray p;
        while (!done && takePacket(&p)) {
            Reader h{p};
            h.num(4);
            const quint32 got = quint32(h.num(4));
            const quint8 flags = quint8(h.num(1));
            const int code = int(h.num(2));
            if (flags & 0x80) {
                if (got == cid) {
                    if (errorCode)
                        *errorCode = code;
                    ok = code == 0;
                    if (ok && reply)
                        *reply = p.mid(11);
                    done = true;
                }
                continue; // the reply of an asynchronous command
            }
            const QByteArray body = p.mid(11);
            // An event while a method runs for us (toString hit a breakpoint): the thread must go on, or the call never ends.
            if (m_invoking && !body.isEmpty() && quint8(body.at(0)) != 0) {
                Reader e{body};
                e.u8();
                e.i32();
                e.u8();
                e.i32();
                const quint64 thread = e.num(m_objectSize);
                sendAsync(ThreadReference, ThreadResume, id(thread, m_objectSize));
                continue;
            }
            m_events << body;
        }
        if (done)
            break;
        if (deadline.hasExpired() || m_sock->state() != QAbstractSocket::ConnectedState)
            break;
        if (m_sock->bytesAvailable() == 0 && !m_sock->waitForReadyRead(int(qMax<qint64>(1, deadline.remainingTime()))))
            break;
        m_buffer += m_sock->readAll();
    }
    if (outer) {
        m_inCommand = false;
        if (!m_events.isEmpty() || m_buffer.size() >= 11)
            QTimer::singleShot(0, this, &DebugWorker::processEvents);
    }
    return ok;
}

void DebugWorker::onReadyRead()
{
    if (m_inCommand)
        return; // the waiting command reads it
    m_buffer += m_sock->readAll();
    processEvents();
}

void DebugWorker::processEvents()
{
    if (m_processing || m_inCommand || m_finished)
        return;
    m_processing = true;
    for (;;) {
        QByteArray p;
        while (takePacket(&p)) {
            Reader h{p};
            h.num(4);
            h.num(4);
            const quint8 flags = quint8(h.num(1));
            if (!(flags & 0x80))
                m_events << p.mid(11);
        }
        if (m_events.isEmpty() || m_finished)
            break;
        handleComposite(m_events.takeFirst());
    }
    m_processing = false;
}

DebugWorker::Location DebugWorker::readLocation(Reader &r) const
{
    Location l;
    l.typeTag = char(r.u8());
    l.classId = r.num(m_refSize);
    l.methodId = r.num(m_methodSize);
    l.index = r.num(8);
    return l;
}

DebugWorker::Value DebugWorker::readUntagged(Reader &r, char tag) const
{
    Value v;
    v.tag = tag;
    v.v = r.num(Jdwp::valueSize(tag, m_objectSize));
    return v;
}

DebugWorker::Value DebugWorker::readTagged(Reader &r) const
{
    return readUntagged(r, char(r.u8()));
}

void DebugWorker::handleComposite(const QByteArray &body)
{
    Reader r{body};
    const int policy = r.u8();
    const int count = r.i32();
    quint64 stopThread = 0;
    QString reason, exceptionText;
    QList<QPair<quint64, QString>> prepared;
    bool died = false;
    for (int i = 0; i < count && !r.bad; ++i) {
        const int kind = r.u8();
        const int request = r.i32();
        switch (kind) {
        case VmStart:
            r.num(m_objectSize);
            break;
        case SingleStep:
        case Breakpoint: {
            const quint64 thread = r.num(m_objectSize);
            readLocation(r);
            if (!stopThread) {
                stopThread = thread;
                reason = kind == SingleStep ? QStringLiteral("step") : QStringLiteral("breakpoint");
            }
            if (kind == Breakpoint)
                reason = QStringLiteral("breakpoint");
            break;
        }
        case Exception: {
            const quint64 thread = r.num(m_objectSize);
            readLocation(r);
            const Value ex = readTagged(r);
            readLocation(r);
            if (!stopThread) {
                stopThread = thread;
                reason = QStringLiteral("exception");
                m_stoppedThread = thread;
                QString type;
                exceptionText = display(ex, &type);
                if (!exceptionText.startsWith(type.section(QLatin1Char('.'), -1)) && !exceptionText.contains(type))
                    exceptionText = type + QStringLiteral(": ") + exceptionText;
            }
            break;
        }
        case ThreadStart:
        case ThreadDeath:
            r.num(m_objectSize);
            break;
        case ClassPrepare: {
            r.num(m_objectSize);
            r.u8();
            const quint64 type = r.num(m_refSize);
            const QString sig = r.str();
            r.i32();
            prepared << qMakePair(type, sig);
            break;
        }
        case ClassUnload:
            r.str();
            break;
        case VmDeath:
            died = true;
            break;
        default:
            r.bad = true; // an event we never asked for: stop reading this packet
            break;
        }
        Q_UNUSED(request)
    }
    for (const auto &p : std::as_const(prepared))
        installFor(p.first, p.second);
    if (died) {
        fail(tr("The program ended."));
        return;
    }
    if (stopThread) {
        if (reason == QLatin1String("step"))
            clearSteps();
        reportStop(stopThread, reason, exceptionText);
        return;
    }
    if (policy != 0) {
        QByteArray rr;
        command(VirtualMachine, VmResume, {}, &rr);
    }
}

// --- reflection ---------------------------------------------------------------------------------------------------------

QString DebugWorker::signature(quint64 type)
{
    if (const auto it = m_sigCache.constFind(type); it != m_sigCache.constEnd())
        return *it;
    QByteArray r;
    QString sig;
    if (command(ReferenceType, RtSignature, id(type, m_refSize), &r)) {
        Reader rd{r};
        sig = rd.str();
    }
    m_sigCache.insert(type, sig);
    return sig;
}

QString DebugWorker::sourceFile(quint64 type)
{
    if (const auto it = m_sourceCache.constFind(type); it != m_sourceCache.constEnd())
        return *it;
    QByteArray r;
    QString src;
    int code = 0;
    if (command(ReferenceType, RtSourceFile, id(type, m_refSize), &r, &code)) {
        Reader rd{r};
        src = rd.str();
    }
    m_sourceCache.insert(type, src);
    return src;
}

QVector<DebugWorker::Method> DebugWorker::methods(quint64 type)
{
    if (const auto it = m_methodCache.constFind(type); it != m_methodCache.constEnd())
        return *it;
    QVector<Method> out;
    QByteArray r;
    if (command(ReferenceType, RtMethods, id(type, m_refSize), &r)) {
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            Method m;
            m.id = rd.num(m_methodSize);
            m.name = rd.str();
            m.signature = rd.str();
            m.modBits = rd.i32();
            out << m;
        }
    }
    m_methodCache.insert(type, out);
    return out;
}

QVector<DebugWorker::LineEntry> DebugWorker::lineTable(quint64 type, quint64 method)
{
    const auto key = qMakePair(type, method);
    if (const auto it = m_lineCache.constFind(key); it != m_lineCache.constEnd())
        return *it;
    QVector<LineEntry> out;
    QByteArray r;
    int code = 0;
    if (command(MethodSet, LineTable, id(type, m_refSize) + id(method, m_methodSize), &r, &code)) {
        Reader rd{r};
        rd.num(8);
        rd.num(8);
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            LineEntry e;
            e.index = rd.num(8);
            e.line = rd.i32();
            out << e;
        }
        std::sort(out.begin(), out.end(), [](const LineEntry &a, const LineEntry &b) { return a.index < b.index; });
    }
    m_lineCache.insert(key, out);
    return out;
}

QVector<DebugWorker::Field> DebugWorker::fields(quint64 type)
{
    if (const auto it = m_fieldCache.constFind(type); it != m_fieldCache.constEnd())
        return *it;
    QVector<Field> out;
    QByteArray r;
    if (command(ReferenceType, RtFields, id(type, m_refSize), &r)) {
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            Field f;
            f.id = rd.num(m_fieldSize);
            f.name = rd.str();
            f.signature = rd.str();
            f.modBits = rd.i32();
            out << f;
        }
    }
    m_fieldCache.insert(type, out);
    return out;
}

quint64 DebugWorker::superclass(quint64 type)
{
    if (const auto it = m_superCache.constFind(type); it != m_superCache.constEnd())
        return *it;
    QByteArray r;
    quint64 sup = 0;
    int code = 0;
    if (command(ClassType, Superclass, id(type, m_refSize), &r, &code)) {
        Reader rd{r};
        sup = rd.num(m_refSize);
    }
    m_superCache.insert(type, sup);
    return sup;
}

QVector<quint64> DebugWorker::interfaces(quint64 type)
{
    if (const auto it = m_ifaceCache.constFind(type); it != m_ifaceCache.constEnd())
        return *it;
    QVector<quint64> out;
    QByteArray r;
    if (command(ReferenceType, RtInterfaces, id(type, m_refSize), &r)) {
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i)
            out << rd.num(m_refSize);
    }
    m_ifaceCache.insert(type, out);
    return out;
}

bool DebugWorker::isInstanceOf(quint64 type, const QString &sig)
{
    if (!m_assignable.contains(type)) {
        QSet<QString> all;
        QList<quint64> todo{type};
        QSet<quint64> seen;
        while (!todo.isEmpty() && seen.size() < 64) {
            const quint64 t = todo.takeFirst();
            if (!t || seen.contains(t))
                continue;
            seen.insert(t);
            all.insert(signature(t));
            todo << superclass(t);
            for (quint64 i : interfaces(t))
                todo << i;
        }
        m_assignable.insert(type, all);
    }
    return m_assignable.value(type).contains(sig);
}

quint64 DebugWorker::typeOf(quint64 object, char *typeTag)
{
    if (const auto it = m_objectType.constFind(object); it != m_objectType.constEnd()) {
        if (typeTag)
            *typeTag = m_objectTypeTag.value(object);
        return *it;
    }
    QByteArray r;
    quint64 type = 0;
    char tag = 1;
    if (command(ObjectReference, OrReferenceType, id(object, m_objectSize), &r)) {
        Reader rd{r};
        tag = char(rd.u8());
        type = rd.num(m_refSize);
    }
    m_objectType.insert(object, type);
    m_objectTypeTag.insert(object, tag);
    if (typeTag)
        *typeTag = tag;
    return type;
}

QString DebugWorker::stringValue(quint64 object)
{
    QByteArray r;
    if (!command(StringReference, StringValue, id(object, m_objectSize), &r))
        return {};
    Reader rd{r};
    return rd.str();
}

int DebugWorker::lineOf(const Location &loc)
{
    const QVector<LineEntry> table = lineTable(loc.classId, loc.methodId);
    int line = 0;
    for (const LineEntry &e : table) {
        if (e.index > loc.index)
            break;
        line = e.line;
    }
    return line;
}

QString DebugWorker::methodName(quint64 type, quint64 method)
{
    for (const Method &m : methods(type))
        if (m.id == method)
            return m.name;
    return QStringLiteral("?");
}

// --- breakpoints --------------------------------------------------------------------------------------------------------

void DebugWorker::clearRequest(int kind, int requestId)
{
    QByteArray q, r;
    q.append(char(kind));
    put(q, quint64(requestId), 4);
    int code = 0;
    command(EventRequest, ErClear, q, &r, &code);
}

void DebugWorker::requestClassPrepare(const QString &sourceName)
{
    if (m_prepareRequests.contains(sourceName))
        return;
    QByteArray q, r;
    q.append(char(ClassPrepare));
    q.append(char(2)); // suspend all: the breakpoints must be in place before the class runs
    put(q, 1, 4);
    q.append(char(12)); // SourceNameMatch
    putString(q, sourceName);
    if (command(EventRequest, ErSet, q, &r)) {
        Reader rd{r};
        m_prepareRequests.insert(sourceName, rd.i32());
    }
}

void DebugWorker::setBreakpoints(const QList<DebugSourceBreakpoints> &specs)
{
    m_specs = specs;
    if (!m_attached || m_finished)
        return;
    // Start over: clear every breakpoint request and place them again (cheap: the line tables are cached).
    for (const Installed &i : std::as_const(m_installed))
        clearRequest(Breakpoint, i.requestId);
    m_installed.clear();
    m_installedTypes.clear();
    QSet<QString> sources;
    for (const DebugSourceBreakpoints &s : std::as_const(m_specs))
        if (!s.lines.isEmpty())
            sources.insert(s.sourceName);
    for (auto it = m_prepareRequests.begin(); it != m_prepareRequests.end();) {
        if (!sources.contains(it.key())) {
            clearRequest(ClassPrepare, it.value());
            it = m_prepareRequests.erase(it);
        } else {
            ++it;
        }
    }
    for (const QString &s : std::as_const(sources))
        requestClassPrepare(s);
    installAllLoaded();
}

void DebugWorker::installAllLoaded()
{
    QSet<QString> prefixes; // "Lcom/example/" (or "L" + no slash for the default package)
    bool defaultPackage = false;
    for (const DebugSourceBreakpoints &s : std::as_const(m_specs)) {
        if (s.lines.isEmpty())
            continue;
        if (s.package.isEmpty())
            defaultPackage = true;
        else
            prefixes.insert(QLatin1Char('L') + QString(s.package).replace(QLatin1Char('.'), QLatin1Char('/')) + QLatin1Char('/'));
    }
    if (prefixes.isEmpty() && !defaultPackage)
        return;
    QByteArray r;
    if (!command(VirtualMachine, AllClasses, {}, &r, nullptr, 15000))
        return;
    Reader rd{r};
    for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
        rd.u8();
        const quint64 type = rd.num(m_refSize);
        const QString sig = rd.str();
        rd.i32();
        bool candidate = defaultPackage && sig.startsWith(QLatin1Char('L')) && !sig.contains(QLatin1Char('/'));
        for (const QString &p : std::as_const(prefixes))
            candidate = candidate || (sig.startsWith(p) && !sig.mid(p.size()).contains(QLatin1Char('/')));
        if (candidate) {
            m_sigCache.insert(type, sig);
            installFor(type, sig);
        }
    }
}

void DebugWorker::installFor(quint64 type, const QString &sig)
{
    if (m_installedTypes.contains(type))
        return;
    QString pkg = sig.mid(1, sig.lastIndexOf(QLatin1Char('/')) - 1);
    if (!sig.contains(QLatin1Char('/')))
        pkg.clear();
    pkg.replace(QLatin1Char('/'), QLatin1Char('.'));
    QString source;
    for (const DebugSourceBreakpoints &s : std::as_const(m_specs)) {
        if (s.lines.isEmpty() || s.package != pkg)
            continue;
        if (source.isNull())
            source = sourceFile(type);
        if (source != s.sourceName)
            continue;
        m_installedTypes.insert(type);
        const QVector<Method> ms = methods(type);
        for (int line : s.lines) {
            bool placed = false;
            for (const Method &m : ms) {
                const QVector<LineEntry> table = lineTable(type, m.id);
                quint64 best = ~quint64(0);
                for (const LineEntry &e : table)
                    if (e.line == line)
                        best = qMin(best, e.index);
                if (best == ~quint64(0))
                    continue;
                QByteArray q, r;
                q.append(char(Breakpoint));
                q.append(char(2)); // suspend all
                put(q, 1, 4);
                q.append(char(7)); // LocationOnly
                q.append(char(1)); // class
                q.append(id(type, m_refSize));
                q.append(id(m.id, m_methodSize));
                put(q, best, 8);
                if (command(EventRequest, ErSet, q, &r)) {
                    Reader rr{r};
                    m_installed << Installed{s.path, line, rr.i32()};
                    placed = true;
                }
            }
            if (placed)
                emit breakpointStatus(s.path, line, true);
        }
    }
}

// --- execution ----------------------------------------------------------------------------------------------------------

void DebugWorker::resume()
{
    if (!m_attached || m_finished)
        return;
    m_suspended = false;
    m_objectType.clear(); // object ids may be reused after the program ran on
    m_objectTypeTag.clear();
    QByteArray r;
    command(VirtualMachine, VmResume, {}, &r);
    emit resumed();
}

void DebugWorker::clearSteps()
{
    for (int id : std::as_const(m_stepRequests))
        clearRequest(SingleStep, id);
    m_stepRequests.clear();
}

void DebugWorker::step(int depth)
{
    if (!m_attached || m_finished || !m_suspended || !m_stoppedThread)
        return;
    clearSteps();
    QByteArray q, r;
    q.append(char(SingleStep));
    q.append(char(2));
    const QStringList &excludes = stepExcludes();
    put(q, quint64(2 + excludes.size()), 4);
    q.append(char(10)); // Step
    q.append(id(m_stoppedThread, m_objectSize));
    put(q, 1, 4); // LINE
    put(q, quint64(depth), 4);
    for (const QString &e : excludes) {
        q.append(char(6)); // ClassExclude
        putString(q, e);
    }
    q.append(char(1)); // Count
    put(q, 1, 4);
    if (command(EventRequest, ErSet, q, &r)) {
        Reader rd{r};
        m_stepRequests << rd.i32();
    }
    resume();
}

void DebugWorker::pause()
{
    if (!m_attached || m_finished || m_suspended)
        return;
    QByteArray r;
    if (!command(VirtualMachine, VmSuspend, {}, &r))
        return;
    // The thread to show: "main" when it runs code, else the first thread with frames.
    quint64 pick = 0;
    for (const auto &t : threads()) {
        QByteArray fr;
        QByteArray q = id(t.first, m_objectSize);
        put(q, 0, 4);
        put(q, 1, 4);
        int code = 0;
        if (!command(ThreadReference, ThreadFrames, q, &fr, &code))
            continue;
        Reader rd{fr};
        if (rd.i32() <= 0)
            continue;
        if (!pick || t.second == QLatin1String("main"))
            pick = t.first;
        if (t.second == QLatin1String("main"))
            break;
    }
    if (pick)
        reportStop(pick, QStringLiteral("pause"), QString());
}

QList<QPair<quint64, QString>> DebugWorker::threads()
{
    QList<QPair<quint64, QString>> out;
    QByteArray r;
    if (!command(VirtualMachine, AllThreads, {}, &r))
        return out;
    Reader rd{r};
    QList<quint64> ids;
    for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i)
        ids << rd.num(m_objectSize);
    for (quint64 t : std::as_const(ids)) {
        QByteArray nr;
        QString name;
        if (command(ThreadReference, ThreadName, id(t, m_objectSize), &nr)) {
            Reader n{nr};
            name = n.str();
        }
        out << qMakePair(t, name);
    }
    return out;
}

QList<DebugFrame> DebugWorker::frames(quint64 thread, QList<quint64> *ids)
{
    QList<DebugFrame> out;
    QByteArray q = id(thread, m_objectSize), r;
    put(q, 0, 4);
    put(q, quint64(quint32(-1)), 4);
    if (!command(ThreadReference, ThreadFrames, q, &r))
        return out;
    Reader rd{r};
    struct Raw { quint64 frame; Location loc; };
    QList<Raw> raw;
    for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
        const quint64 frame = rd.num(m_frameSize);
        raw << Raw{frame, readLocation(rd)};
    }
    for (int i = 0; i < raw.size() && i < kMaxFrames; ++i) {
        const Raw &f = raw.at(i);
        DebugFrame d;
        d.frameId = f.frame;
        const QString sig = signature(f.loc.classId);
        d.className = Jdwp::typeName(sig);
        d.method = methodName(f.loc.classId, f.loc.methodId);
        d.sourceName = sourceFile(f.loc.classId);
        const int slash = sig.lastIndexOf(QLatin1Char('/'));
        d.pkgPath = slash > 1 ? sig.mid(1, slash - 1) : QString();
        d.line = lineOf(f.loc);
        out << d;
        if (ids)
            *ids << f.frame;
    }
    return out;
}

void DebugWorker::reportStop(quint64 thread, const QString &reason, const QString &exception)
{
    m_suspended = true;
    m_stoppedThread = thread;
    m_frameIds.clear();
    m_frames = frames(thread, &m_frameIds);
    DebugStop s;
    s.thread = thread;
    s.reason = reason;
    s.exception = exception;
    s.frames = m_frames;
    s.threads = threads();
    for (const auto &t : std::as_const(s.threads))
        if (t.first == thread)
            s.threadName = t.second;
    emit stopped(s);
}

void DebugWorker::selectThread(quint64 thread)
{
    if (!m_suspended || m_finished)
        return;
    reportStop(thread, QStringLiteral("thread"), QString());
}

// --- values -------------------------------------------------------------------------------------------------------------

bool DebugWorker::invoke(quint64 object, quint64 thread, const QString &name, const QString &sig, Value *result)
{
    if (!thread || !object)
        return false;
    // The method, from the object's class upwards.
    quint64 type = typeOf(object);
    quint64 owner = 0, method = 0;
    for (int depth = 0; type && depth < 20 && !method; ++depth, type = superclass(type))
        for (const Method &m : methods(type))
            if (m.name == name && (sig.isEmpty() ? m.signature.startsWith(QLatin1String("()")) : m.signature == sig) && !(m.modBits & 0x0400)) {
                owner = type;
                method = m.id;
                break;
            }
    if (!method)
        return false;
    QByteArray q = id(object, m_objectSize) + id(thread, m_objectSize) + id(owner, m_refSize) + id(method, m_methodSize), r;
    put(q, 0, 4); // no arguments
    put(q, 1, 4); // INVOKE_SINGLE_THREADED
    m_invoking = true;
    int code = 0;
    const bool ok = command(ObjectReference, InvokeMethod, q, &r, &code, 2500);
    m_invoking = false;
    if (!ok)
        return false;
    Reader rd{r};
    *result = readTagged(rd);
    const Value ex = readTagged(rd);
    return ex.v == 0;
}

QString DebugWorker::display(const Value &v, QString *type)
{
    if (!Jdwp::isReferenceTag(v.tag)) {
        if (type)
            *type = Jdwp::typeName(QString(QLatin1Char(v.tag)));
        return primitive(v.tag, v.v);
    }
    if (v.v == 0)
        return QStringLiteral("null");
    if (v.tag == 's') {
        if (type)
            *type = QStringLiteral("java.lang.String");
        return quoted(stringValue(v.v));
    }
    const quint64 t = typeOf(v.v);
    const QString sig = signature(t);
    const QString name = Jdwp::typeName(sig);
    if (type)
        *type = name;
    if (v.tag == '[') {
        QByteArray r;
        int n = 0;
        if (command(ArrayReference, ArrayLength, id(v.v, m_objectSize), &r)) {
            Reader rd{r};
            n = rd.i32();
        }
        QString elem = name;
        elem.chop(2);
        return QStringLiteral("%1[%2]").arg(elem).arg(n);
    }
    // Boxed primitives: their value field.
    static const QStringList boxes = {QStringLiteral("Ljava/lang/Integer;"), QStringLiteral("Ljava/lang/Long;"), QStringLiteral("Ljava/lang/Short;"),
                                      QStringLiteral("Ljava/lang/Byte;"), QStringLiteral("Ljava/lang/Character;"), QStringLiteral("Ljava/lang/Boolean;"),
                                      QStringLiteral("Ljava/lang/Float;"), QStringLiteral("Ljava/lang/Double;")};
    if (boxes.contains(sig)) {
        for (const Field &f : fields(t))
            if (f.name == QLatin1String("value")) {
                QByteArray q = id(v.v, m_objectSize), r;
                put(q, 1, 4);
                q.append(id(f.id, m_fieldSize));
                if (command(ObjectReference, OrGetValues, q, &r)) {
                    Reader rd{r};
                    rd.i32();
                    const Value inner = readTagged(rd);
                    return primitive(inner.tag, inner.v);
                }
            }
    }
    // Everything else as its toString(), run in the stopped thread.
    Value s;
    if (m_stoppedThread && invoke(v.v, m_stoppedThread, QStringLiteral("toString"), QStringLiteral("()Ljava/lang/String;"), &s) && s.v) {
        QString text = stringValue(s.v);
        if (text.size() > 300)
            text = text.left(300) + QStringLiteral("…");
        return text;
    }
    return QStringLiteral("%1@%2").arg(name.section(QLatin1Char('.'), -1)).arg(v.v);
}

DebugVariable DebugWorker::describe(const QString &name, const Value &v, const QString &declaredType)
{
    DebugVariable d;
    d.name = name;
    d.tag = v.tag;
    QString type;
    d.value = display(v, &type);
    d.type = type.isEmpty() ? declaredType : type;
    // Objects open on demand; boxed numbers and booleans already show their value.
    static const QStringList boxes = {QStringLiteral("java.lang.Integer"), QStringLiteral("java.lang.Long"), QStringLiteral("java.lang.Short"),
                                      QStringLiteral("java.lang.Byte"), QStringLiteral("java.lang.Character"), QStringLiteral("java.lang.Boolean"),
                                      QStringLiteral("java.lang.Float"), QStringLiteral("java.lang.Double")};
    if (Jdwp::isReferenceTag(v.tag) && v.v != 0 && v.tag != 's' && !boxes.contains(d.type))
        d.objectId = v.v;
    return d;
}

QList<DebugVariable> DebugWorker::frameVariables(quint64 thread, quint64 frameId, const Location &loc)
{
    QList<DebugVariable> out;
    QByteArray q = id(thread, m_objectSize) + id(frameId, m_frameSize), r;
    if (command(StackFrame, SfThisObject, q, &r)) {
        Reader rd{r};
        const Value self = readTagged(rd);
        if (self.v) {
            out << describe(QStringLiteral("this"), self, QString());
            // A Kotlin script's top-level vals are fields of the script object: list them as variables.
            if (sourceFile(loc.classId).endsWith(QLatin1String(".kts")))
                for (const DebugVariable &v : children(self.v))
                    if (!v.name.startsWith(QLatin1Char('$')))
                        out << v;
        }
    }
    QByteArray vr;
    int code = 0;
    if (!command(MethodSet, VariableTable, id(loc.classId, m_refSize) + id(loc.methodId, m_methodSize), &vr, &code)) {
        if (code == kAbsentInformation) {
            DebugVariable note;
            note.name = tr("(no local variable information)");
            note.value = tr("compiled without -g");
            out << note;
        }
        return out;
    }
    Reader rd{vr};
    const int argCount = rd.i32();
    QList<SlotVar> live;
    for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
        SlotVar s;
        s.codeIndex = rd.num(8);
        s.name = rd.str();
        s.signature = rd.str();
        s.length = rd.i32();
        s.slot = rd.i32();
        if (loc.index < s.codeIndex || loc.index >= s.codeIndex + quint64(s.length))
            continue;
        if (s.name.startsWith(QLatin1String("$i$")) || s.name == QLatin1String("this") || s.name.startsWith(QLatin1String("$$")))
            continue; // Kotlin's inline markers
        live << s;
    }
    std::stable_sort(live.begin(), live.end(), [argCount](const SlotVar &a, const SlotVar &b) {
        const bool aa = a.slot < argCount, ab = b.slot < argCount;
        if (aa != ab)
            return aa;
        return aa ? a.slot < b.slot : a.codeIndex < b.codeIndex;
    });
    if (live.isEmpty())
        return out;
    QByteArray gq = id(thread, m_objectSize) + id(frameId, m_frameSize), gr;
    put(gq, quint64(live.size()), 4);
    for (const SlotVar &s : std::as_const(live)) {
        put(gq, quint64(s.slot), 4);
        gq.append(s.signature.isEmpty() ? 'L' : s.signature.at(0).toLatin1());
    }
    if (!command(StackFrame, SfGetValues, gq, &gr))
        return out;
    Reader g{gr};
    const int n = g.i32();
    for (int i = 0; i < n && i < live.size() && !g.bad; ++i)
        out << describe(live.at(i).name, readTagged(g), Jdwp::typeName(live.at(i).signature));
    return out;
}

void DebugWorker::selectFrame(int token, quint64 thread, int frameIndex)
{
    if (!m_suspended || m_finished)
        return;
    if (thread != m_stoppedThread) {
        m_stoppedThread = thread;
        m_frameIds.clear();
        m_frames = frames(thread, &m_frameIds);
    }
    if (frameIndex < 0 || frameIndex >= m_frameIds.size()) {
        emit variables(token, {});
        return;
    }
    // Locations again: the frame list holds only display data.
    QByteArray q = id(thread, m_objectSize), r;
    put(q, quint64(frameIndex), 4);
    put(q, 1, 4);
    if (!command(ThreadReference, ThreadFrames, q, &r)) {
        emit variables(token, {});
        return;
    }
    Reader rd{r};
    rd.i32();
    const quint64 frame = rd.num(m_frameSize);
    const Location loc = readLocation(rd);
    emit variables(token, frameVariables(thread, frame, loc));
}

QList<DebugVariable> DebugWorker::children(quint64 objectId)
{
    QList<DebugVariable> out;
    char tag = 0;
    const quint64 type = typeOf(objectId, &tag);
    const QString sig = signature(type);
    auto arrayElements = [this, &out](quint64 array, const QString &arraySig, const QString &prefix) {
        QByteArray r;
        int n = 0;
        if (command(ArrayReference, ArrayLength, id(array, m_objectSize), &r)) {
            Reader rd{r};
            n = rd.i32();
        }
        const int take = qMin(n, kMaxChildren);
        if (take <= 0)
            return;
        QByteArray q = id(array, m_objectSize), ar;
        put(q, 0, 4);
        put(q, quint64(take), 4);
        if (!command(ArrayReference, ArrayGetValues, q, &ar))
            return;
        Reader rd{ar};
        const char elemTag = char(rd.u8());
        const int count = rd.i32();
        const bool primitiveElems = !Jdwp::isReferenceTag(elemTag);
        const QString elemType = Jdwp::typeName(arraySig.mid(1));
        for (int i = 0; i < count && !rd.bad; ++i)
            out << describe(prefix.arg(i), primitiveElems ? readUntagged(rd, elemTag) : readTagged(rd), elemType);
        if (n > take) {
            DebugVariable more;
            more.name = QStringLiteral("…");
            more.value = tr("%1 more").arg(n - take);
            out << more;
        }
    };
    if (sig.startsWith(QLatin1Char('['))) {
        arrayElements(objectId, sig, QStringLiteral("[%1]"));
        return out;
    }
    // Collections and maps: their elements (toArray / entrySet), not their internals.
    if (m_stoppedThread && isInstanceOf(type, QStringLiteral("Ljava/util/Collection;"))) {
        Value arr;
        if (invoke(objectId, m_stoppedThread, QStringLiteral("toArray"), QStringLiteral("()[Ljava/lang/Object;"), &arr) && arr.v) {
            arrayElements(arr.v, QStringLiteral("[Ljava/lang/Object;"), QStringLiteral("[%1]"));
            return out;
        }
    }
    if (m_stoppedThread && isInstanceOf(type, QStringLiteral("Ljava/util/Map;"))) {
        Value set, arr;
        if (invoke(objectId, m_stoppedThread, QStringLiteral("entrySet"), QStringLiteral("()Ljava/util/Set;"), &set) && set.v
            && invoke(set.v, m_stoppedThread, QStringLiteral("toArray"), QStringLiteral("()[Ljava/lang/Object;"), &arr) && arr.v) {
            QByteArray r;
            int n = 0;
            if (command(ArrayReference, ArrayLength, id(arr.v, m_objectSize), &r)) {
                Reader rd{r};
                n = rd.i32();
            }
            QByteArray q = id(arr.v, m_objectSize), ar;
            put(q, 0, 4);
            put(q, quint64(qMin(n, kMaxChildren / 2)), 4);
            if (n > 0 && command(ArrayReference, ArrayGetValues, q, &ar)) {
                Reader rd{ar};
                rd.u8();
                for (int i = 0, c = rd.i32(); i < c && !rd.bad; ++i) {
                    const Value entry = readTagged(rd);
                    Value k, v;
                    if (!invoke(entry.v, m_stoppedThread, QStringLiteral("getKey"), QString(), &k)
                        || !invoke(entry.v, m_stoppedThread, QStringLiteral("getValue"), QString(), &v))
                        continue;
                    QString keyType;
                    out << describe(display(k, &keyType), v, QString());
                }
            }
            return out;
        }
    }
    // Fields, the class's own first, then inherited ones; statics left out.
    int invokes = 0;
    quint64 t = type;
    for (int depth = 0; t && depth < 12 && out.size() < kMaxChildren; ++depth, t = superclass(t)) {
        const QString s = signature(t);
        if (s == QLatin1String("Ljava/lang/Object;"))
            break;
        QVector<Field> list;
        for (const Field &f : fields(t))
            if (!(f.modBits & 0x0008))
                list << f;
        if (list.isEmpty())
            continue;
        QByteArray q = id(objectId, m_objectSize), r;
        put(q, quint64(list.size()), 4);
        for (const Field &f : std::as_const(list))
            q.append(id(f.id, m_fieldSize));
        if (!command(ObjectReference, OrGetValues, q, &r))
            continue;
        Reader rd{r};
        const int n = rd.i32();
        for (int i = 0; i < n && i < list.size() && !rd.bad; ++i) {
            const Value v = readTagged(rd);
            // toString() for every field is slow on big objects: past a budget objects show as Type@id.
            const quint64 keepThread = m_stoppedThread;
            if (++invokes > kMaxInvokesPerRequest)
                m_stoppedThread = 0;
            out << describe(list.at(i).name, v, Jdwp::typeName(list.at(i).signature));
            m_stoppedThread = keepThread;
        }
    }
    return out;
}

void DebugWorker::expand(int token, quint64 objectId)
{
    if (!m_suspended || m_finished) {
        emit variables(token, {});
        return;
    }
    emit variables(token, children(objectId));
}

// `name`, `a.b.c`, `list[2]`, `user.getName()`: locals of the frame (or fields of `this`), then fields, elements and
// methods without arguments.
void DebugWorker::evaluate(int token, quint64 thread, int frameIndex, const QString &expression)
{
    if (!m_suspended || m_finished) {
        emit evaluated(token, {}, tr("The program is running."));
        return;
    }
    static const QRegularExpression part(QStringLiteral(R"(^\s*(?:\.?\s*(\w+)\s*(\(\s*\))?|\[\s*(\d+)\s*\]))"));
    QString rest = expression.trimmed();
    QRegularExpressionMatch m = part.match(rest);
    if (!m.hasMatch() || m.captured(1).isEmpty()) {
        emit evaluated(token, {}, tr("Only names, fields (a.b), elements (a[0]) and calls without arguments (a.size()) can be evaluated."));
        return;
    }
    // The first name: a local, else a field of this.
    QByteArray q = id(thread, m_objectSize), r;
    put(q, quint64(qMax(0, frameIndex)), 4);
    put(q, 1, 4);
    if (!command(ThreadReference, ThreadFrames, q, &r)) {
        emit evaluated(token, {}, tr("The frame is gone."));
        return;
    }
    Reader rd{r};
    rd.i32();
    const quint64 frame = rd.num(m_frameSize);
    const Location loc = readLocation(rd);
    const QString first = m.captured(1);
    Value current;
    bool found = false;
    QByteArray vr;
    if (command(MethodSet, VariableTable, id(loc.classId, m_refSize) + id(loc.methodId, m_methodSize), &vr)) {
        Reader v{vr};
        v.i32();
        for (int i = 0, n = v.i32(); i < n && !v.bad && !found; ++i) {
            const quint64 ci = v.num(8);
            const QString name = v.str();
            const QString sig = v.str();
            const int length = v.i32(), slot = v.i32();
            if (name != first || loc.index < ci || loc.index >= ci + quint64(length))
                continue;
            QByteArray gq = id(thread, m_objectSize) + id(frame, m_frameSize), gr;
            put(gq, 1, 4);
            put(gq, quint64(slot), 4);
            gq.append(sig.isEmpty() ? 'L' : sig.at(0).toLatin1());
            if (command(StackFrame, SfGetValues, gq, &gr)) {
                Reader g{gr};
                g.i32();
                current = readTagged(g);
                found = true;
            }
        }
    }
    QByteArray tr2;
    Value self;
    if (command(StackFrame, SfThisObject, id(thread, m_objectSize) + id(frame, m_frameSize), &tr2)) {
        Reader t{tr2};
        self = readTagged(t);
    }
    auto fieldOf = [this](const Value &obj, const QString &name, Value *out) {
        for (quint64 t = typeOf(obj.v); t; t = superclass(t))
            for (const Field &f : fields(t))
                if (f.name == name) {
                    QByteArray fq = id(obj.v, m_objectSize), fr;
                    put(fq, 1, 4);
                    fq.append(id(f.id, m_fieldSize));
                    if (!command(ObjectReference, OrGetValues, fq, &fr))
                        return false;
                    Reader rr{fr};
                    rr.i32();
                    *out = readTagged(rr);
                    return true;
                }
        return false;
    };
    if (!found && first == QLatin1String("this") && self.v) {
        current = self;
        found = true;
    }
    if (!found && self.v)
        found = fieldOf(self, first, &current);
    if (!found) {
        emit evaluated(token, {}, tr("No variable named \"%1\" here.").arg(first));
        return;
    }
    QString label = first;
    rest = rest.mid(m.capturedLength());
    if (!m.captured(2).isEmpty()) {
        emit evaluated(token, {}, tr("Start with a variable name."));
        return;
    }
    while (!rest.trimmed().isEmpty()) {
        m = part.match(rest);
        if (!m.hasMatch()) {
            emit evaluated(token, {}, tr("Cannot read \"%1\".").arg(rest.trimmed()));
            return;
        }
        rest = rest.mid(m.capturedLength());
        if (!Jdwp::isReferenceTag(current.tag) || current.v == 0) {
            emit evaluated(token, {}, tr("%1 is not an object.").arg(label));
            return;
        }
        if (!m.captured(3).isEmpty()) {
            const int index = m.captured(3).toInt();
            label += QStringLiteral("[%1]").arg(index);
            QByteArray aq = id(current.v, m_objectSize), ar;
            put(aq, quint64(index), 4);
            put(aq, 1, 4);
            if (current.tag != '[' || !command(ArrayReference, ArrayGetValues, aq, &ar)) {
                // A list: get(i) is a method with an argument, so read it through toArray().
                Value arr;
                if (!invoke(current.v, thread, QStringLiteral("toArray"), QStringLiteral("()[Ljava/lang/Object;"), &arr) || !arr.v) {
                    emit evaluated(token, {}, tr("%1 has no element %2.").arg(label).arg(index));
                    return;
                }
                aq = id(arr.v, m_objectSize);
                put(aq, quint64(index), 4);
                put(aq, 1, 4);
                if (!command(ArrayReference, ArrayGetValues, aq, &ar)) {
                    emit evaluated(token, {}, tr("Index %1 is out of range.").arg(index));
                    return;
                }
            }
            Reader a{ar};
            const char et = char(a.u8());
            a.i32();
            current = Jdwp::isReferenceTag(et) ? readTagged(a) : readUntagged(a, et);
            continue;
        }
        const QString name = m.captured(1);
        label += QLatin1Char('.') + name + (m.captured(2).isEmpty() ? QString() : QStringLiteral("()"));
        if (!m.captured(2).isEmpty()) {
            Value res;
            if (!invoke(current.v, thread, name, QString(), &res)) {
                emit evaluated(token, {}, tr("Could not call %1().").arg(name));
                return;
            }
            current = res;
        } else if (!fieldOf(current, name, &current)) {
            // Kotlin properties are fields behind getters: try the getter.
            Value res;
            const QString getter = QStringLiteral("get") + name.left(1).toUpper() + name.mid(1);
            if (!invoke(current.v, thread, getter, QString(), &res)) {
                emit evaluated(token, {}, tr("%1 has no field %2.").arg(label.section(QLatin1Char('.'), 0, -2), name));
                return;
            }
            current = res;
        }
    }
    emit evaluated(token, describe(label, current, QString()), QString());
}
