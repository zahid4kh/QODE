#include "ktor/KtorLive.h"

#include <QDeadlineTimer>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTcpSocket>

#include <algorithm>

namespace {

// --- JDWP (https://docs.oracle.com/en/java/javase/21/docs/specs/jdwp/jdwp-protocol.html): big-endian packets, a reply
// carries the id of its command. Command sets / commands used here:
enum : int {
    VirtualMachine = 1, ClassesBySignature = 2, Dispose = 6, IDSizes = 7,
    ReferenceType = 2, Signature = 1, ClassLoader = 2, Fields = 4, Methods = 5, SourceFile = 7, Instances = 16,
    ClassType = 3, Superclass = 1,
    MethodSet = 6, LineTable = 1,
    ObjectReference = 9, RefType = 1, GetValues = 2,
    StringReference = 10, StringValue = 1,
    ArrayReference = 13, ArrayLength = 1, ArrayGetValues = 2,
};

struct Value
{
    char tag = 0;
    quint64 v = 0;
    bool object() const { return v != 0 && QByteArrayView("Lstglc[").contains(tag); }
};

int valueSize(char tag, int objectSize)
{
    switch (tag) {
    case 'B': case 'Z': return 1;
    case 'C': case 'S': return 2;
    case 'F': case 'I': return 4;
    case 'D': case 'J': return 8;
    case 'V': return 0;
    default: return objectSize; // L s t g l c [
    }
}

struct Reader
{
    const QByteArray &d;
    int pos = 0;
    bool bad = false;

    quint64 num(int size)
    {
        if (size < 0 || pos + size > d.size()) {
            bad = true;
            return 0;
        }
        quint64 v = 0;
        for (int i = 0; i < size; ++i)
            v = (v << 8) | quint8(d.at(pos + i));
        pos += size;
        return v;
    }
    qint32 i32() { return qint32(num(4)); }
    QString str()
    {
        const qint32 n = i32();
        if (bad || n < 0 || pos + n > d.size()) {
            bad = true;
            return {};
        }
        const QString s = QString::fromUtf8(d.constData() + pos, n);
        pos += n;
        return s;
    }
};

void put(QByteArray &out, quint64 v, int size)
{
    for (int i = size - 1; i >= 0; --i)
        out.append(char((v >> (8 * i)) & 0xff));
}

void putString(QByteArray &out, const QString &s)
{
    const QByteArray u = s.toUtf8();
    put(out, quint64(u.size()), 4);
    out.append(u);
}

class Jdwp
{
public:
    explicit Jdwp(int timeoutMs) : m_deadline(timeoutMs) {}

    QString error;

    bool open(int port)
    {
        m_sock.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
        if (!m_sock.waitForConnected(remaining())) {
            error = QStringLiteral("could not connect to the debug port %1: %2").arg(port).arg(m_sock.errorString());
            return false;
        }
        const QByteArray hello("JDWP-Handshake");
        m_sock.write(hello);
        QByteArray answer;
        if (!readExact(hello.size(), &answer) || answer != hello) {
            error = QStringLiteral("the debug port did not answer the JDWP handshake (another debugger may be attached)");
            return false;
        }
        QByteArray r;
        if (!command(VirtualMachine, IDSizes, {}, &r))
            return false;
        Reader rd{r};
        m_fieldSize = rd.i32();
        m_methodSize = rd.i32();
        m_objectSize = rd.i32();
        m_refSize = rd.i32();
        return !rd.bad;
    }

    void close()
    {
        if (m_sock.state() == QAbstractSocket::ConnectedState) {
            QByteArray r;
            command(VirtualMachine, Dispose, {}, &r); // detach: the agent goes back to listening for the next read
            m_sock.disconnectFromHost();
        }
    }

    bool command(int set, int cmd, const QByteArray &data, QByteArray *reply, int *errorCode = nullptr)
    {
        const quint32 id = ++m_id;
        QByteArray packet;
        put(packet, quint64(11 + data.size()), 4);
        put(packet, id, 4);
        packet.append(char(0));
        packet.append(char(set));
        packet.append(char(cmd));
        packet.append(data);
        m_sock.write(packet);
        for (;;) {
            QByteArray head;
            if (!readExact(11, &head))
                return false;
            Reader h{head};
            const quint32 len = quint32(h.num(4));
            const quint32 got = quint32(h.num(4));
            const quint8 flags = quint8(h.num(1));
            const int code = int(h.num(2)); // error code of a reply; command set + command of an event
            QByteArray body;
            if (len < 11 || !readExact(int(len - 11), &body))
                return false;
            if (!(flags & 0x80) || got != id)
                continue; // an event the VM sends on its own (VMStart …)
            if (errorCode)
                *errorCode = code;
            if (code != 0) {
                if (!errorCode)
                    error = QStringLiteral("JDWP error %1 (command %2/%3)").arg(code).arg(set).arg(cmd);
                return false;
            }
            *reply = body;
            return true;
        }
    }

    QByteArray objectId(quint64 v) const
    {
        QByteArray b;
        put(b, v, m_objectSize);
        return b;
    }
    QByteArray refId(quint64 v) const
    {
        QByteArray b;
        put(b, v, m_refSize);
        return b;
    }

    QVector<quint64> classes(const QString &signature)
    {
        QVector<quint64> out;
        QByteArray q, r;
        putString(q, signature);
        if (!command(VirtualMachine, ClassesBySignature, q, &r))
            return out;
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            rd.num(1);
            const quint64 type = rd.num(m_refSize);
            rd.i32();
            out << type;
        }
        return out;
    }

    // Instances still reachable (JDWP never returns garbage).
    QVector<quint64> instances(quint64 type)
    {
        QVector<quint64> out;
        QByteArray q = refId(type), r;
        put(q, 0, 4); // all of them
        if (!command(ReferenceType, Instances, q, &r))
            return out;
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            rd.num(1);
            out << rd.num(m_objectSize);
        }
        return out;
    }

    quint64 typeOf(quint64 obj)
    {
        if (const auto it = m_typeOf.constFind(obj); it != m_typeOf.constEnd())
            return *it;
        QByteArray r;
        quint64 type = 0;
        if (command(ObjectReference, RefType, objectId(obj), &r)) {
            Reader rd{r};
            rd.num(1);
            type = rd.num(m_refSize);
        }
        m_typeOf.insert(obj, type);
        return type;
    }

    QString signature(quint64 type)
    {
        if (const auto it = m_signature.constFind(type); it != m_signature.constEnd())
            return *it;
        QByteArray r;
        QString sig;
        if (command(ReferenceType, Signature, refId(type), &r)) {
            Reader rd{r};
            sig = rd.str();
        }
        m_signature.insert(type, sig);
        return sig;
    }

    quint64 superclass(quint64 type)
    {
        if (const auto it = m_super.constFind(type); it != m_super.constEnd())
            return *it;
        QByteArray r;
        int code = 0;
        quint64 sup = 0;
        if (command(ClassType, Superclass, refId(type), &r, &code)) {
            Reader rd{r};
            sup = rd.num(m_refSize);
        }
        m_super.insert(type, sup);
        return sup;
    }

    bool inherits(quint64 type, const QString &sig)
    {
        for (int depth = 0; type && depth < 16; ++depth, type = superclass(type))
            if (signature(type) == sig)
                return true;
        return false;
    }

    struct Field
    {
        quint64 id = 0;
        QString name, sig;
        bool isStatic = false;
    };

    const QVector<Field> &fields(quint64 type)
    {
        auto it = m_fields.find(type);
        if (it != m_fields.end())
            return *it;
        QVector<Field> out;
        QByteArray r;
        int code = 0;
        if (command(ReferenceType, Fields, refId(type), &r, &code)) {
            Reader rd{r};
            for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
                Field f;
                f.id = rd.num(m_fieldSize);
                f.name = rd.str();
                f.sig = rd.str();
                f.isStatic = rd.i32() & 0x0008;
                out << f;
            }
        }
        return *m_fields.insert(type, out);
    }

    QVector<Value> values(quint64 obj, const QVector<quint64> &fieldIds)
    {
        QVector<Value> out;
        if (fieldIds.isEmpty())
            return out;
        QByteArray q = objectId(obj), r;
        put(q, quint64(fieldIds.size()), 4);
        for (quint64 f : fieldIds)
            put(q, f, m_fieldSize);
        int code = 0;
        if (!command(ObjectReference, GetValues, q, &r, &code))
            return out;
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            Value v;
            v.tag = char(rd.num(1));
            v.v = rd.num(valueSize(v.tag, m_objectSize));
            out << v;
        }
        return out;
    }

    // An instance field by name, looked up through the superclasses.
    Value field(quint64 obj, const QString &name)
    {
        if (!obj)
            return {};
        for (quint64 type = typeOf(obj); type; type = superclass(type)) {
            for (const Field &f : fields(type)) {
                if (!f.isStatic && f.name == name) {
                    const QVector<Value> v = values(obj, {f.id});
                    return v.isEmpty() ? Value() : v.first();
                }
            }
        }
        return {};
    }

    // Every object-valued instance field of the object's own class (a lambda's captured variables).
    QVector<Value> objectFields(quint64 obj)
    {
        QVector<quint64> ids;
        for (const Field &f : fields(typeOf(obj)))
            if (!f.isStatic && f.sig.startsWith(QLatin1Char('L')))
                ids << f.id;
        QVector<Value> out;
        for (const Value &v : values(obj, ids))
            if (v.object())
                out << v;
        return out;
    }

    QString string(const Value &v)
    {
        if (!v.object())
            return {};
        QByteArray r;
        if (!command(StringReference, StringValue, objectId(v.v), &r))
            return {};
        Reader rd{r};
        return rd.str();
    }

    QVector<Value> array(quint64 arr, int limit)
    {
        QVector<Value> out;
        QByteArray r;
        if (!command(ArrayReference, ArrayLength, objectId(arr), &r))
            return out;
        Reader lr{r};
        const int length = std::min(lr.i32(), limit);
        if (length <= 0)
            return out;
        QByteArray q = objectId(arr);
        put(q, 0, 4);
        put(q, quint64(length), 4);
        if (!command(ArrayReference, ArrayGetValues, q, &r))
            return out;
        Reader rd{r};
        const char tag = char(rd.num(1));
        const bool tagged = QByteArrayView("Lstglc[").contains(tag);
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            Value v;
            v.tag = tagged ? char(rd.num(1)) : tag;
            v.v = rd.num(valueSize(v.tag, m_objectSize));
            out << v;
        }
        return out;
    }

    // The elements of a java.util.List as Kotlin and Ktor build them.
    QVector<Value> list(const Value &v, int limit = 4096)
    {
        if (!v.object())
            return {};
        const QString sig = signature(typeOf(v.v));
        if (sig == QLatin1String("Ljava/util/ArrayList;")) {
            const int size = int(field(v.v, QStringLiteral("size")).v);
            QVector<Value> all = array(field(v.v, QStringLiteral("elementData")).v, std::min(size, limit));
            all.resize(std::min<qsizetype>(all.size(), size));
            return all;
        }
        if (sig == QLatin1String("Ljava/util/concurrent/CopyOnWriteArrayList;"))
            return array(field(v.v, QStringLiteral("array")).v, limit);
        if (sig == QLatin1String("Ljava/util/Arrays$ArrayList;"))
            return array(field(v.v, QStringLiteral("a")).v, limit);
        if (sig == QLatin1String("Ljava/util/ImmutableCollections$ListN;"))
            return array(field(v.v, QStringLiteral("elements")).v, limit);
        if (sig == QLatin1String("Ljava/util/Collections$SingletonList;"))
            return {field(v.v, QStringLiteral("element"))};
        if (sig == QLatin1String("Ljava/util/ImmutableCollections$List12;")) {
            QVector<Value> out{field(v.v, QStringLiteral("e0"))};
            const Value e1 = field(v.v, QStringLiteral("e1"));
            if (e1.object() && signature(typeOf(e1.v)) != QLatin1String("Ljava/lang/Object;")) // EMPTY marker
                out << e1;
            return out;
        }
        if (sig.startsWith(QLatin1String("Ljava/util/Collections$Unmodifiable"))
            || sig.startsWith(QLatin1String("Ljava/util/Collections$Synchronized")))
            return list(field(v.v, QStringLiteral("list")), limit);
        return {}; // kotlin.collections.EmptyList and anything unknown
    }

    // Class name of the loader that defined the type ("" for the bootstrap loader).
    QString loaderOf(quint64 type)
    {
        QByteArray r;
        int code = 0;
        if (!command(ReferenceType, ClassLoader, refId(type), &r, &code))
            return {};
        Reader rd{r};
        const quint64 loader = rd.num(m_objectSize);
        if (!loader)
            return {};
        const QString sig = signature(typeOf(loader));
        return sig.mid(1, sig.size() - 2).replace(QLatin1Char('/'), QLatin1Char('.'));
    }

    QString sourceFile(quint64 type)
    {
        QByteArray r;
        int code = 0;
        if (!command(ReferenceType, SourceFile, refId(type), &r, &code))
            return {}; // ABSENT_INFORMATION: compiled without debug information
        Reader rd{r};
        return rd.str();
    }

    // Lowest line of the lambda's body (invokeSuspend for a suspend lambda, else invoke); 0 when unknown. Lines of
    // inlined functions are mapped past the end of the file, so the lowest is the body's own first line.
    int firstLine(quint64 type, bool *callable)
    {
        *callable = false;
        QByteArray r;
        int code = 0;
        if (!command(ReferenceType, Methods, refId(type), &r, &code))
            return 0;
        QVector<quint64> wanted;
        Reader rd{r};
        for (int i = 0, n = rd.i32(); i < n && !rd.bad; ++i) {
            const quint64 id = rd.num(m_methodSize);
            const QString name = rd.str();
            rd.str();
            rd.i32();
            if (name == QLatin1String("invokeSuspend") || name == QLatin1String("invoke"))
                wanted << id;
        }
        *callable = !wanted.isEmpty();
        int best = 0;
        for (quint64 m : std::as_const(wanted)) {
            QByteArray q = refId(type), lr;
            put(q, m, m_methodSize);
            if (!command(MethodSet, LineTable, q, &lr, &code))
                continue;
            Reader t{lr};
            t.num(8);
            t.num(8);
            for (int i = 0, n = t.i32(); i < n && !t.bad; ++i) {
                t.num(8);
                const int line = t.i32();
                if (line > 0 && (best == 0 || line < best))
                    best = line;
            }
        }
        return best;
    }

private:
    int remaining() const { return int(std::max<qint64>(1, m_deadline.remainingTime())); }

    bool readExact(int n, QByteArray *out)
    {
        while (m_in.size() < n) {
            if (m_deadline.hasExpired() || !m_sock.waitForReadyRead(remaining())) {
                if (error.isEmpty())
                    error = QStringLiteral("the server's debug port stopped answering");
                return false;
            }
            m_in.append(m_sock.readAll());
        }
        *out = m_in.left(n);
        m_in.remove(0, n);
        return true;
    }

    QTcpSocket m_sock;
    QDeadlineTimer m_deadline;
    QByteArray m_in;
    quint32 m_id = 0;
    int m_fieldSize = 8, m_methodSize = 8, m_objectSize = 8, m_refSize = 8;
    QHash<quint64, quint64> m_typeOf, m_super;
    QHash<quint64, QString> m_signature;
    QHash<quint64, QVector<Field>> m_fields;
};

// --- The routing tree

struct Context
{
    QStringList segments;
    bool trailingSlash = false;
    QString method;
    QStringList auth, conditions;
    KtorRoute::Kind kind = KtorRoute::Http;
};

QString simpleName(const QString &sig)
{
    QString s = sig.mid(1);
    s.chop(1);
    return s.section(QLatin1Char('/'), -1);
}

bool isLibrary(const QString &sig)
{
    static const char *const prefixes[] = {"Ljava/", "Ljavax/", "Ljdk/", "Lsun/", "Lkotlin/", "Lkotlinx/", "Lio/ktor/", "Lorg/slf4j/"};
    for (const char *p : prefixes)
        if (sig.startsWith(QLatin1String(p)))
            return true;
    return false;
}

class TreeReader
{
public:
    explicit TreeReader(Jdwp &j) : m_j(j) {}

    QVector<KtorLiveRoute> routes;

    void walk(quint64 node, Context ctx, int depth)
    {
        if (!node || depth > 64 || m_seen.contains(node) || routes.size() > 5000)
            return;
        m_seen.insert(node);
        applySelector(m_j.field(node, QStringLiteral("selector")), ctx);
        const QVector<Value> handlers = m_j.list(m_j.field(node, QStringLiteral("handlers")), 8);
        if (!handlers.isEmpty())
            addRoute(ctx, handlers.first());
        for (const Value &child : m_j.list(m_j.field(node, QStringLiteral("childList"))))
            walk(child.v, ctx, depth + 1);
    }

private:
    QString str(quint64 obj, const char *name) { return m_j.string(m_j.field(obj, QLatin1String(name))); }

    QString contentType(const Value &ct)
    {
        const QString type = str(ct.v, "contentType"), sub = str(ct.v, "contentSubtype");
        return type.isEmpty() ? QString() : type + QLatin1Char('/') + sub;
    }

    void applySelector(const Value &sel, Context &ctx)
    {
        if (!sel.object())
            return;
        const quint64 type = m_j.typeOf(sel.v);
        const QString sig = m_j.signature(type);
        const QString name = simpleName(sig);
        const quint64 s = sel.v;
        if (name == QLatin1String("PathSegmentConstantRouteSelector")) {
            ctx.segments << str(s, "value");
        } else if (name == QLatin1String("PathSegmentParameterRouteSelector") || name == QLatin1String("PathSegmentOptionalParameterRouteSelector")) {
            const bool optional = name.contains(QLatin1String("Optional"));
            ctx.segments << str(s, "prefix") + QLatin1Char('{') + str(s, "name") + (optional ? QStringLiteral("?}") : QStringLiteral("}")) + str(s, "suffix");
        } else if (name == QLatin1String("PathSegmentWildcardRouteSelector")) {
            ctx.segments << QStringLiteral("*");
        } else if (name == QLatin1String("PathSegmentTailcardRouteSelector")) {
            ctx.segments << str(s, "prefix") + QLatin1Char('{') + str(s, "name") + QStringLiteral("...}");
        } else if (name == QLatin1String("PathSegmentRegexRouteSelector")) {
            const Value regex = m_j.field(s, QStringLiteral("regex"));
            const Value pattern = m_j.field(regex.v, QStringLiteral("nativePattern"));
            ctx.segments << QLatin1Char('(') + str(pattern.v, "pattern") + QLatin1Char(')');
        } else if (name == QLatin1String("RootRouteSelector")) {
            for (const Value &p : m_j.list(m_j.field(s, QStringLiteral("parts"))))
                ctx.segments << m_j.string(p);
        } else if (name == QLatin1String("TrailingSlashRouteSelector")) {
            ctx.trailingSlash = true;
        } else if (name == QLatin1String("HttpMethodRouteSelector")) {
            ctx.method = str(m_j.field(s, QStringLiteral("method")).v, "value");
        } else if (name == QLatin1String("AuthenticationRouteSelector")) {
            for (const Value &n : m_j.list(m_j.field(s, QStringLiteral("names"))))
                ctx.auth << m_j.string(n); // null: the default provider
        } else if (name == QLatin1String("HttpHeaderRouteSelector")) {
            const QString header = str(s, "name"), value = str(s, "value");
            if (header.compare(QLatin1String("Upgrade"), Qt::CaseInsensitive) == 0 && value.contains(QLatin1String("websocket"), Qt::CaseInsensitive))
                ctx.kind = KtorRoute::WebSocket;
            else if (!(header.compare(QLatin1String("Connection"), Qt::CaseInsensitive) == 0 && value.contains(QLatin1String("Upgrade"), Qt::CaseInsensitive)))
                ctx.conditions << QStringLiteral("header %1: %2").arg(header, value);
        } else if (name == QLatin1String("WebSocketProtocolsSelector")) {
            ctx.kind = KtorRoute::WebSocket;
            if (const QString p = str(s, "requiredProtocol"); !p.isEmpty())
                ctx.conditions << QStringLiteral("protocol %1").arg(p);
        } else if (name == QLatin1String("HttpAcceptRouteSelector")) {
            const QString ct = contentType(m_j.field(s, QStringLiteral("contentType")));
            if (ct == QLatin1String("text/event-stream"))
                ctx.kind = KtorRoute::Sse;
            else if (!ct.isEmpty())
                ctx.conditions << QStringLiteral("accept %1").arg(ct);
        } else if (name == QLatin1String("HttpMultiAcceptRouteSelector")) {
            QStringList types;
            for (const Value &ct : m_j.list(m_j.field(s, QStringLiteral("contentTypes"))))
                types << contentType(ct);
            ctx.conditions << QStringLiteral("accept %1").arg(types.join(QStringLiteral(", ")));
        } else if (name == QLatin1String("ConstantParameterRouteSelector")) {
            ctx.conditions << QStringLiteral("?%1=%2").arg(str(s, "name"), str(s, "value"));
        } else if (name == QLatin1String("ParameterRouteSelector")) {
            ctx.conditions << QStringLiteral("?%1").arg(str(s, "name"));
        } else if (name == QLatin1String("OptionalParameterRouteSelector")) {
            ctx.conditions << QStringLiteral("?%1 (optional)").arg(str(s, "name"));
        } else if (sig.startsWith(QLatin1String("Lio/ktor/server/http/content/"))) {
            ctx.kind = KtorRoute::Static; // staticResources' own filter (TailcardSelector): part of the static route
        } else if (!m_j.inherits(type, QStringLiteral("Lio/ktor/server/routing/TransparentRouteSelector;"))) {
            // A selector of a plugin or of the application: shown by name (transparent ones only tag a subtree).
            QString shown = name.section(QLatin1Char('$'), -1);
            shown.remove(QStringLiteral("RouteSelector"));
            shown.remove(QStringLiteral("Selector"));
            if (!shown.isEmpty())
                ctx.conditions << shown;
        }
    }

    void addRoute(const Context &ctx, const Value &handler)
    {
        KtorLiveRoute r;
        r.method = ctx.method;
        r.auth = ctx.auth;
        r.conditions = ctx.conditions;
        r.kind = ctx.kind;
        QString path = QLatin1Char('/') + ctx.segments.join(QLatin1Char('/'));
        if (ctx.trailingSlash && !ctx.segments.isEmpty())
            path += QLatin1Char('/');
        r.path = path;

        // Plugins wrap the application's lambda in their own (webSocket, sse, get<Resource>): look through the captured
        // variables, breadth first, for a lambda of the project. The outermost class tells the plugin.
        const QString outer = m_j.signature(m_j.typeOf(handler.v));
        if (r.kind == KtorRoute::Http) {
            if (outer.startsWith(QLatin1String("Lio/ktor/server/websocket/")))
                r.kind = KtorRoute::WebSocket;
            else if (outer.startsWith(QLatin1String("Lio/ktor/server/sse/")))
                r.kind = KtorRoute::Sse;
            else if (outer.startsWith(QLatin1String("Lio/ktor/server/http/content/")))
                r.kind = KtorRoute::Static;
            else if (outer.startsWith(QLatin1String("Lio/ktor/server/plugins/swagger/")) || outer.startsWith(QLatin1String("Lio/ktor/server/plugins/openapi/")))
                r.kind = KtorRoute::Docs;
        }
        QVector<QPair<quint64, int>> queue{{handler.v, 0}};
        QSet<quint64> visited;
        for (int i = 0; i < queue.size() && i < 24; ++i) {
            const auto [obj, depth] = queue.at(i);
            if (visited.contains(obj))
                continue;
            visited.insert(obj);
            const quint64 type = m_j.typeOf(obj);
            const QString sig = m_j.signature(type);
            if (!isLibrary(sig)) {
                bool callable = false;
                const int line = m_j.firstLine(type, &callable);
                if (callable) {
                    r.handlerClass = sig.mid(1, sig.size() - 2).replace(QLatin1Char('/'), QLatin1Char('.'));
                    r.sourceFile = m_j.sourceFile(type);
                    r.line = line;
                    r.loader = m_j.loaderOf(type);
                    break;
                }
            }
            if (depth < 3)
                for (const Value &v : m_j.objectFields(obj))
                    queue.append({v.v, depth + 1});
        }
        routes << r;
    }

    Jdwp &m_j;
    QSet<quint64> m_seen;
};

// --- Joining with the source scan

QString normalized(QString path)
{
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

// The method the scan writes for a route kind ("WS" / "SSE" are labels, the tree has GET).
QString shownMethod(const KtorLiveRoute &r)
{
    switch (r.kind) {
    case KtorRoute::WebSocket: return QStringLiteral("WS");
    case KtorRoute::Sse: return QStringLiteral("SSE");
    default: return r.method.isEmpty() ? QStringLiteral("ANY") : r.method;
    }
}

// com.example.RoutingKt$configureRouting$1$1 -> configureRouting; com.example.Api$1 -> Api.
QString functionOf(const QString &cls)
{
    const QStringList parts = cls.section(QLatin1Char('.'), -1).split(QLatin1Char('$'));
    for (int i = 1; i < parts.size(); ++i) {
        bool number = false;
        parts.at(i).toInt(&number);
        if (!number && !parts.at(i).isEmpty())
            return parts.at(i);
    }
    QString owner = parts.value(0);
    if (owner.endsWith(QLatin1String("Kt")))
        owner.chop(2);
    return owner;
}

QString detailOf(const KtorRoute &r, const QStringList &conditions)
{
    QStringList detail;
    if (r.kind != KtorRoute::Http)
        detail << r.kindName();
    if (!r.auth.isEmpty()) {
        QStringList shown;
        for (const QString &a : r.auth)
            shown << (a.isEmpty() ? QStringLiteral("default") : a);
        detail << QStringLiteral("auth: ") + shown.join(QStringLiteral(", "));
    }
    detail << conditions;
    if (r.runtimeOnly)
        detail << QStringLiteral("runtime only");
    if (r.notServed)
        detail << QStringLiteral("not served");
    return detail.join(QStringLiteral(" · "));
}

} // namespace

int KtorLiveResult::codeReloadable() const
{
    for (const KtorLiveRoute &r : routes)
        if (!r.loader.isEmpty())
            return r.loader.startsWith(QLatin1String("io.ktor.server.engine.OverridingClassLoader")) ? 1 : 0;
    return -1;
}

namespace KtorLive {

KtorLiveResult read(int port, int timeoutMs)
{
    KtorLiveResult out;
    Jdwp j(timeoutMs);
    if (!j.open(port)) {
        out.error = j.error;
        return out;
    }

    // The application the server runs now. After an auto-reload the previous one can still be reachable for a moment.
    QSet<quint64> apps;
    for (const auto &[sig, name] : {std::pair{"Lio/ktor/server/engine/EmbeddedServer;", "applicationInstance"},
                                     std::pair{"Lio/ktor/server/engine/ApplicationEngineEnvironmentReloading;", "_applicationInstance"}})
        for (quint64 type : j.classes(QLatin1String(sig)))
            for (quint64 server : j.instances(type))
                if (const Value app = j.field(server, QLatin1String(name)); app.object())
                    apps.insert(app.v);

    QVector<quint64> roots;
    QVector<quint64> types = j.classes(QStringLiteral("Lio/ktor/server/routing/RoutingRoot;"));
    if (types.isEmpty())
        types = j.classes(QStringLiteral("Lio/ktor/server/routing/Routing;")); // Ktor 2
    for (quint64 type : std::as_const(types))
        for (quint64 root : j.instances(type))
            if (apps.isEmpty() || apps.contains(j.field(root, QStringLiteral("application")).v))
                roots << root;

    TreeReader tree(j);
    for (quint64 root : std::as_const(roots))
        tree.walk(root, Context(), 0);
    if (!j.error.isEmpty() && tree.routes.isEmpty()) {
        out.error = j.error;
        j.close();
        return out;
    }
    j.close();
    out.ok = true;
    out.routes = tree.routes;
    return out;
}

KtorScan merge(const KtorScan &code, const KtorLiveResult &live, const Resolver &resolve)
{
    if (!live.ok)
        return code;
    KtorScan out = code;
    out.routes.clear();
    out.http.endpoints.clear();
    out.live = true;

    QVector<bool> used(code.routes.size(), false);
    auto findCode = [&](const KtorLiveRoute &r) -> int {
        const QString path = normalized(r.path);
        const QString method = shownMethod(r);
        // Static content and API docs serve a subtree ({static-content-path-parameter...}): the declaring call is the
        // route of the scan whose path is a prefix of the served one.
        const bool subtree = r.kind == KtorRoute::Static || r.kind == KtorRoute::Docs;
        for (int i = 0; i < code.routes.size(); ++i) {
            const KtorRoute &c = code.routes.at(i);
            if (c.kind != r.kind || (!subtree && used.at(i)))
                continue;
            const QString cp = normalized(c.ep.path);
            if (subtree ? (path == cp || path.startsWith(cp == QLatin1String("/") ? cp : cp + QLatin1Char('/'))) : (cp == path && c.ep.method == method))
                return i;
        }
        // A path built from a string template (get("/greeting/$lang") in a loop): the scan has the template, the tree one
        // route per value. Each matches the template.
        static const QRegularExpression templateVar(QStringLiteral(R"(\$\{[^}]*\}|\$\w+)"));
        for (int i = 0; i < code.routes.size(); ++i) {
            const KtorRoute &c = code.routes.at(i);
            if (c.kind != r.kind || c.ep.method != method || !c.ep.path.contains(QLatin1Char('$')))
                continue;
            QString pattern;
            int last = 0;
            const QString cp = normalized(c.ep.path);
            for (QRegularExpressionMatchIterator it = templateVar.globalMatch(cp); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                pattern += QRegularExpression::escape(cp.mid(last, m.capturedStart() - last)) + QStringLiteral(".+?");
                last = int(m.capturedEnd());
            }
            pattern += QRegularExpression::escape(cp.mid(last));
            if (QRegularExpression(QRegularExpression::anchoredPattern(pattern)).match(path).hasMatch())
                return i;
        }
        return -1;
    };

    QSet<int> subtreeShown;
    for (const KtorLiveRoute &r : live.routes) {
        const int ci = findCode(r);
        KtorRoute route;
        if (ci >= 0) {
            const bool subtree = r.kind == KtorRoute::Static || r.kind == KtorRoute::Docs;
            if (subtree && subtreeShown.contains(ci))
                continue; // one row per staticResources(…) / swaggerUI(…) call
            if (subtree)
                subtreeShown.insert(ci);
            used[ci] = true;
            route = code.routes.at(ci);
            route.mounted = true;
            if (!subtree)
                route.ep.path = r.path;
        } else {
            route.kind = r.kind;
            route.runtimeOnly = true;
            route.ep.method = shownMethod(r);
            route.ep.path = r.path;
            const QString pkgPath = r.handlerClass.section(QLatin1Char('.'), 0, -2).replace(QLatin1Char('.'), QLatin1Char('/'));
            route.ep.file = r.sourceFile.isEmpty() || !resolve ? QString() : resolve(pkgPath, r.sourceFile);
            route.ep.line = std::max(1, r.line);
            route.ep.controller = r.handlerClass.isEmpty() ? QStringLiteral("Ktor") : functionOf(r.handlerClass);
            // Path parameters for the Requests tab.
            static const QRegularExpression param(QStringLiteral(R"(\{([\w-]+)(\?|\.\.\.)?\})"));
            for (QRegularExpressionMatchIterator it = param.globalMatch(r.path); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                SpringParam p;
                p.kind = SpringParam::Path;
                p.name = m.captured(1);
                p.type = QStringLiteral("String");
                p.required = m.captured(2).isEmpty();
                route.ep.params << p;
            }
        }
        route.auth = r.auth; // the tree knows the authenticate(…) blocks for certain
        route.ep.handler = detailOf(route, r.conditions);
        out.routes << route;
    }
    // Declared in code, not served: a routing function nobody calls, a condition the server never met …
    for (int i = 0; i < code.routes.size(); ++i) {
        if (used.at(i))
            continue;
        KtorRoute route = code.routes.at(i);
        route.notServed = true;
        route.ep.handler = detailOf(route, {});
        out.routes << route;
    }

    std::stable_sort(out.routes.begin(), out.routes.end(), [](const KtorRoute &a, const KtorRoute &b) {
        if (a.ep.file.isEmpty() != b.ep.file.isEmpty())
            return !a.ep.file.isEmpty();
        if (a.ep.file != b.ep.file)
            return a.ep.file < b.ep.file;
        return a.ep.line < b.ep.line;
    });
    for (const KtorRoute &r : std::as_const(out.routes))
        if (r.kind == KtorRoute::Http && !r.notServed)
            out.http.endpoints << r.ep;
    return out;
}

} // namespace KtorLive
