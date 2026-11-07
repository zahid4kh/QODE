#include "McpClient.h"

#include "platform/Platform.h"
#include "platform/ProcessTree.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTimer>

namespace mcp {

namespace {
constexpr int kStartTimeoutMs = 180000; // uvx / npx may download the server on its first start; some wait for their app
constexpr const char *kProtocol = "2025-06-18";

// Ends a server's process tree without waiting for it: ProcessTree::stop, kill two seconds later if it is still there.
// The process leaves its client (whose destructor would otherwise kill only the main process and wait for it).
void endProcess(QProcess *p, QObject *owner)
{
    p->disconnect(owner);
    p->setParent(nullptr);
    if (p->state() == QProcess::NotRunning) {
        p->deleteLater();
        return;
    }
    p->closeWriteChannel(); // stdio servers end when their input does
    const qint64 pid = p->processId();
    QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
    QTimer::singleShot(500, p, [p, pid] {
        if (p->state() != QProcess::NotRunning)
            ProcessTree::stop(pid);
    });
    QTimer::singleShot(2500, p, [p, pid] {
        if (p->state() != QProcess::NotRunning)
            ProcessTree::kill(pid);
    });
}
} // namespace

Client::Client(const ServerConfig &config, const QString &projectRoot, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_root(projectRoot)
    , m_startTimer(new QTimer(this))
{
    m_startTimer->setSingleShot(true);
    connect(m_startTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting)
            fail(tr("The server did not answer within %1 seconds.").arg(kStartTimeoutMs / 1000));
    });
}

Client::~Client()
{
    failPending(QStringLiteral("the MCP server was removed")); // ends a callTool() that is waiting
    if (m_proc)
        endProcess(m_proc, this);
    m_proc = nullptr;
}

void Client::setState(State state)
{
    m_state = state;
    emit changed();
}

void Client::start()
{
    stop();
    ++m_generation;
    m_error.clear();
    m_tools = QJsonArray();
    m_instructions.clear();
    m_serverTitle.clear();
    m_buffer.clear();
    m_stderr.clear();
    m_queued.clear();
    if (m_config.command.trimmed().isEmpty()) {
        fail(tr("No command is set."));
        return;
    }
    const QProcessEnvironment env = environment(m_config);
    QString cwd = expand(m_config.cwd, env);
    if (cwd.isEmpty())
        cwd = m_root;
    if (!cwd.isEmpty() && !QFileInfo(cwd).isDir()) {
        fail(tr("The working folder %1 does not exist.").arg(cwd));
        return;
    }
    QStringList args;
    for (const QString &a : std::as_const(m_config.args))
        args << expand(a, env);

    auto *p = m_proc = new QProcess(this);
    p->setProcessEnvironment(env);
    if (!cwd.isEmpty())
        p->setWorkingDirectory(cwd);
    Platform::setCommand(p, expand(m_config.command, env), args); // Windows: npx -> npx.cmd, through cmd
    ProcessTree::track(p); // own process group: stop() ends what it started too, and so does QODE ending
    connect(p, &QProcess::readyReadStandardOutput, this, &Client::readOut);
    connect(p, &QProcess::readyReadStandardError, this, [this, p] {
        m_stderr += p->readAllStandardError();
        if (m_stderr.size() > 32000)
            m_stderr = m_stderr.right(16000);
    });
    connect(p, &QProcess::started, this, [this, p] {
        for (const QByteArray &line : std::as_const(m_queued))
            p->write(line);
        m_queued.clear();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || m_proc != p)
            return;
        m_proc = nullptr;
        p->deleteLater();
        fail(tr("Could not start “%1”: %2. Check the command, or give its full path.").arg(m_config.command, p->errorString()));
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        if (m_proc != p)
            return;
        readOut();
        m_proc = nullptr;
        p->deleteLater();
        QString last = QString::fromUtf8(m_stderr).trimmed();
        last = last.mid(last.lastIndexOf(QLatin1Char('\n'), -2) + 1).right(300); // its last log line usually says why
        QString why = st == QProcess::CrashExit ? tr("The server crashed.") : tr("The server exited with code %1.").arg(code);
        if (!last.isEmpty())
            why += QLatin1Char(' ') + last;
        fail(why);
    });

    setState(State::Starting);
    m_startTimer->start(kStartTimeoutMs);
    const int gen = m_generation;
    request(QStringLiteral("initialize"),
            QJsonObject{{"protocolVersion", QLatin1String(kProtocol)},
                        {"capabilities", QJsonObject()},
                        {"clientInfo", QJsonObject{{"name", "QODE"}, {"version", QCoreApplication::applicationVersion()}}}},
            [this, gen](const QJsonObject &result, const QString &error) {
                if (gen != m_generation)
                    return;
                if (!error.isEmpty()) {
                    fail(tr("The server refused to start: %1").arg(error));
                    return;
                }
                const QJsonObject info = result.value("serverInfo").toObject();
                m_serverTitle = (info.value("title").toString().isEmpty() ? info.value("name").toString() : info.value("title").toString());
                if (!info.value("version").toString().isEmpty())
                    m_serverTitle += QLatin1Char(' ') + info.value("version").toString();
                m_instructions = result.value("instructions").toString();
                notify(QStringLiteral("notifications/initialized"));
                listTools();
            });
    p->start();
}

void Client::stop()
{
    m_startTimer->stop();
    ++m_generation; // the start's callbacks are not run
    failPending(tr("The server was stopped."));
    if (m_proc) {
        QProcess *p = m_proc;
        m_proc = nullptr;
        endProcess(p, this);
    }
    if (m_state != State::Stopped && m_state != State::Failed) {
        m_state = State::Stopped;
        emit changed();
    }
}

void Client::fail(const QString &error)
{
    m_startTimer->stop();
    m_error = error;
    m_state = State::Failed; // before the callbacks below, which would otherwise report the failure again
    ++m_generation;
    failPending(error);
    if (m_proc) {
        QProcess *p = m_proc;
        m_proc = nullptr;
        endProcess(p, this);
    }
    emit changed();
}

void Client::failPending(const QString &error)
{
    const auto pending = std::exchange(m_pending, {});
    for (const Callback &cb : pending)
        cb({}, error);
}

void Client::listTools(const QString &cursor)
{
    if (cursor.isEmpty())
        m_collect = QJsonArray();
    QJsonObject params;
    if (!cursor.isEmpty())
        params.insert("cursor", cursor);
    const int gen = m_generation;
    request(QStringLiteral("tools/list"), params, [this, gen](const QJsonObject &result, const QString &error) {
        if (gen != m_generation)
            return;
        if (!error.isEmpty()) {
            if (m_state == State::Starting)
                fail(tr("Listing the tools failed: %1").arg(error));
            return;
        }
        for (const QJsonValue &t : result.value("tools").toArray())
            m_collect.append(t);
        const QString next = result.value("nextCursor").toString();
        if (!next.isEmpty() && m_collect.size() < 1000) {
            listTools(next);
            return;
        }
        m_tools = m_collect;
        m_startTimer->stop();
        setState(State::Connected);
    });
}

int Client::request(const QString &method, const QJsonObject &params, Callback done)
{
    const int id = ++m_nextId;
    m_pending.insert(id, std::move(done));
    QJsonObject msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.isEmpty() || method != QLatin1String("tools/list"))
        msg.insert("params", params);
    send(msg);
    return id;
}

void Client::notify(const QString &method, const QJsonObject &params)
{
    QJsonObject msg{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isEmpty())
        msg.insert("params", params);
    send(msg);
}

void Client::send(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (m_proc && m_proc->state() == QProcess::Running)
        m_proc->write(line);
    else if (m_proc) // QProcess drops what is written before it runs: kept until `started`
        m_queued << line;
}

void Client::readOut()
{
    if (!m_proc)
        return;
    m_buffer += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (!line.startsWith('{'))
            continue; // a server that logs to stdout: not protocol
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (!o.isEmpty())
            handle(o);
    }
    if (m_buffer.size() > 64 * 1024 * 1024)
        m_buffer.clear();
}

void Client::handle(const QJsonObject &message)
{
    const QJsonValue id = message.value("id");
    const QString method = message.value("method").toString();
    if (method.isEmpty()) { // an answer
        const Callback cb = m_pending.take(id.toInt());
        if (!cb)
            return;
        const QJsonObject err = message.value("error").toObject();
        if (message.contains("error"))
            cb({}, err.value("message").toString(tr("unknown error")));
        else
            cb(message.value("result").toObject(), QString());
        return;
    }
    if (id.isUndefined() || id.isNull()) { // a notification
        if (method == QLatin1String("notifications/tools/list_changed") && m_state == State::Connected)
            listTools();
        return;
    }
    // A request from the server. QODE offers no sampling, roots or elicitation; ping is answered.
    QJsonObject reply{{"jsonrpc", "2.0"}, {"id", id}};
    if (method == QLatin1String("ping"))
        reply.insert("result", QJsonObject());
    else
        reply.insert("error", QJsonObject{{"code", -32601}, {"message", QStringLiteral("QODE does not support %1").arg(method)}});
    send(reply);
}

QJsonObject Client::callTool(const QString &tool, const QJsonObject &args, int timeoutMs, const std::function<bool()> &cancelled, QString *error)
{
    if (m_state != State::Connected) {
        *error = m_error.isEmpty() ? tr("the MCP server %1 is not connected").arg(m_config.name) : m_error;
        return {};
    }
    QPointer<Client> guard(this);
    bool done = false;
    QJsonObject result;
    QString err;
    QEventLoop loop;
    const int id = request(QStringLiteral("tools/call"), QJsonObject{{"name", tool}, {"arguments", args}},
                           [&](const QJsonObject &r, const QString &e) {
                               result = r;
                               err = e;
                               done = true;
                               loop.quit();
                           });
    QTimer limit, poll;
    limit.setSingleShot(true);
    bool timedOut = false;
    connect(&limit, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    connect(&poll, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled())
            loop.quit();
    });
    limit.start(timeoutMs);
    poll.start(100);
    if (!done)
        loop.exec();
    if (!guard) {
        *error = QStringLiteral("the MCP server was removed");
        return {};
    }
    if (!done) {
        m_pending.remove(id);
        notify(QStringLiteral("notifications/cancelled"),
               QJsonObject{{"requestId", id}, {"reason", timedOut ? QStringLiteral("timed out") : QStringLiteral("cancelled by the user")}});
        *error = timedOut ? QStringLiteral("no answer from the MCP server within %1 s").arg(timeoutMs / 1000) : QStringLiteral("cancelled by the user");
        return {};
    }
    *error = err;
    return result;
}

} // namespace mcp
