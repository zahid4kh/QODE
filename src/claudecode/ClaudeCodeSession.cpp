#include "ClaudeCodeSession.h"

#include "claudecode/ClaudeCodeCli.h"
#include "platform/Platform.h"

#include <QJsonDocument>
#include <QProcess>

namespace claudecode {

Session::Session(QObject *parent)
    : QObject(parent)
{
}

Session::~Session()
{
    stop();
}

bool Session::isRunning() const
{
    return m_proc != nullptr;
}

void Session::start(const Options &o)
{
    stop();
    const QString exe = executable();
    if (exe.isEmpty()) {
        emit exited(-1, tr("Claude Code is not installed."));
        return;
    }
    QStringList args{QStringLiteral("-p"),
                     QStringLiteral("--output-format"), QStringLiteral("stream-json"),
                     QStringLiteral("--input-format"), QStringLiteral("stream-json"),
                     QStringLiteral("--verbose"),
                     QStringLiteral("--include-partial-messages"),
                     QStringLiteral("--permission-prompt-tool"), QStringLiteral("stdio"),
                     // Lets the mode menu switch to "Bypass permissions" later; the session still starts in the mode below.
                     QStringLiteral("--allow-dangerously-skip-permissions")};
    if (!o.model.isEmpty())
        args << QStringLiteral("--model") << o.model;
    if (!o.permissionMode.isEmpty())
        args << QStringLiteral("--permission-mode") << o.permissionMode;
    if (!o.resume.isEmpty())
        args << QStringLiteral("--resume") << o.resume;

    m_buffer.clear();
    m_stderr.clear();
    m_sessionId = o.resume;
    m_stopping = false;
    auto *p = m_proc = new QProcess(this);
    p->setProcessEnvironment(environment(o.root));
    p->setWorkingDirectory(o.root);
    Platform::setCommand(p, exe, args); // Windows: npm's claude.cmd goes through cmd
    connect(p, &QProcess::readyReadStandardOutput, this, &Session::readOut);
    connect(p, &QProcess::readyReadStandardError, this, [this, p] {
        m_stderr += p->readAllStandardError();
        if (m_stderr.size() > 16000)
            m_stderr = m_stderr.right(8000);
    });
    connect(p, &QProcess::started, this, [this, p] {
        for (const QByteArray &line : std::as_const(m_pending))
            p->write(line);
        m_pending.clear();
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        readOut();
        const bool clean = m_stopping || (st == QProcess::NormalExit && code == 0);
        const QString err = clean ? QString() : QString::fromUtf8(m_stderr).trimmed().right(1500);
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        if (!m_stopping)
            emit exited(st == QProcess::NormalExit ? code : -1, err);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        m_pending.clear();
        p->deleteLater();
        emit exited(-1, p->errorString());
    });
    // The initialize request comes first (the SDK does the same); its answer lists the commands and models.
    m_initId = QStringLiteral("qode-init-%1").arg(++m_nextId);
    request(QJsonObject{{"subtype", "initialize"}}, m_initId);
    p->start();
}

void Session::stop()
{
    if (!m_proc)
        return;
    m_stopping = true;
    QProcess *p = m_proc;
    m_proc = nullptr;
    m_pending.clear();
    p->disconnect(this);
    p->closeWriteChannel(); // end of input: the CLI saves the conversation and exits
    if (!p->waitForFinished(1500)) {
        p->terminate();
        if (!p->waitForFinished(1000))
            Platform::killTree(p); // Windows: also the node behind claude.cmd
    }
    p->deleteLater();
}

void Session::write(const QJsonObject &o)
{
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    if (m_proc && m_proc->state() == QProcess::Running)
        m_proc->write(line);
    else if (m_proc)
        m_pending << line;
}

void Session::request(const QJsonObject &req, const QString &id)
{
    write(QJsonObject{{"type", "control_request"},
                      {"request_id", id.isEmpty() ? QStringLiteral("qode-%1").arg(++m_nextId) : id},
                      {"request", req}});
}

void Session::sendUser(const QJsonValue &content)
{
    write(QJsonObject{{"type", "user"},
                      {"message", QJsonObject{{"role", "user"}, {"content", content}}},
                      {"parent_tool_use_id", QJsonValue::Null},
                      {"session_id", m_sessionId}});
}

void Session::interrupt()
{
    request(QJsonObject{{"subtype", "interrupt"}});
}

void Session::setModel(const QString &model)
{
    QJsonObject r{{"subtype", "set_model"}};
    r.insert(QStringLiteral("model"), model.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(model));
    request(r);
}

void Session::setPermissionMode(const QString &mode)
{
    request(QJsonObject{{"subtype", "set_permission_mode"}, {"mode", mode}});
}

QString Session::query(const QJsonObject &req)
{
    const QString id = QStringLiteral("qode-%1").arg(++m_nextId);
    request(req, id);
    return id;
}

void Session::answerPermission(const QString &requestId, const QJsonObject &decision)
{
    write(QJsonObject{{"type", "control_response"},
                      {"response", QJsonObject{{"subtype", "success"}, {"request_id", requestId}, {"response", decision}}}});
}

void Session::readOut()
{
    if (!m_proc)
        return;
    m_buffer += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (line.isEmpty() || !line.startsWith('{'))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (!o.isEmpty())
            handle(o);
    }
}

void Session::handle(const QJsonObject &o)
{
    const QString type = o.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("control_response")) {
        const QJsonObject r = o.value(QStringLiteral("response")).toObject();
        const QString id = r.value(QStringLiteral("request_id")).toString();
        if (id == m_initId)
            emit initialized(r.value(QStringLiteral("response")).toObject());
        else
            emit answered(id, r.value(QStringLiteral("response")).toObject(),
                          r.value(QStringLiteral("subtype")).toString() == QLatin1String("error")
                              ? r.value(QStringLiteral("error")).toString(tr("failed")) : QString());
        return;
    }
    if (type == QLatin1String("control_request")) {
        const QString id = o.value(QStringLiteral("request_id")).toString();
        const QJsonObject req = o.value(QStringLiteral("request")).toObject();
        if (req.value(QStringLiteral("subtype")).toString() == QLatin1String("can_use_tool")) {
            emit permissionRequested(id, req);
            return;
        }
        // Hooks, SDK MCP servers and dialogs are not offered: say so, or the CLI would wait for an answer forever.
        write(QJsonObject{{"type", "control_response"},
                          {"response", QJsonObject{{"subtype", "error"}, {"request_id", id}, {"error", "not supported by QODE"}}}});
        return;
    }
    if (type == QLatin1String("control_cancel_request")) {
        emit permissionCancelled(o.value(QStringLiteral("request_id")).toString());
        return;
    }
    const QString sid = o.value(QStringLiteral("session_id")).toString();
    if (!sid.isEmpty())
        m_sessionId = sid;
    emit messageReceived(o);
}

} // namespace claudecode
