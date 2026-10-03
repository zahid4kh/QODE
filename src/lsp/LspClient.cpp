#include "LspClient.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kMaxLogLines = 200;
}

LspClient::LspClient(const QString &program, const QStringList &arguments, const QString &rootPath, const QJsonObject &initializationOptions,
                     QObject *parent)
    : QObject(parent), m_proc(new QProcess(this)), m_root(rootPath), m_initOptions(initializationOptions)
{
    m_proc->setProgram(program);
    m_proc->setArguments(arguments);
    if (QFileInfo(rootPath).isDir())
        m_proc->setWorkingDirectory(rootPath);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &LspClient::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, [this] {
        const QStringList lines = QString::fromUtf8(m_proc->readAllStandardError()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        m_log << lines;
        while (m_log.size() > kMaxLogLines)
            m_log.removeFirst();
    });
    connect(m_proc, &QProcess::finished, this, &LspClient::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fail(tr("Could not start %1").arg(m_proc->program()));
    });
    connect(m_proc, &QProcess::started, this, [this] {
        QJsonObject textDocument{
            {QStringLiteral("synchronization"), QJsonObject{{QStringLiteral("didSave"), true}}},
            {QStringLiteral("publishDiagnostics"), QJsonObject{{QStringLiteral("relatedInformation"), false}}},
            {QStringLiteral("diagnostic"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}}, // pull model (Kotlin)
            {QStringLiteral("hover"), QJsonObject{{QStringLiteral("contentFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}}}},
            {QStringLiteral("formatting"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("definition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("codeAction"),
             QJsonObject{{QStringLiteral("codeActionLiteralSupport"),
                          QJsonObject{{QStringLiteral("codeActionKind"),
                                       QJsonObject{{QStringLiteral("valueSet"),
                                                    QJsonArray{QStringLiteral("quickfix"), QStringLiteral("refactor"), QStringLiteral("source"),
                                                               QStringLiteral("source.organizeImports")}}}}}}}},
            {QStringLiteral("completion"),
             QJsonObject{{QStringLiteral("contextSupport"), true},
                         {QStringLiteral("completionItem"),
                          QJsonObject{{QStringLiteral("snippetSupport"), true},
                                      {QStringLiteral("deprecatedSupport"), true},
                                      {QStringLiteral("labelDetailsSupport"), true},
                                      {QStringLiteral("documentationFormat"), QJsonArray{QStringLiteral("plaintext")}}}}}},
        };
        QJsonObject params{
            {QStringLiteral("processId"), int(QCoreApplication::applicationPid())},
            {QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("QODE")},
                                                      {QStringLiteral("version"), QStringLiteral(QODE_VERSION)}}},
            {QStringLiteral("rootUri"), QUrl::fromLocalFile(m_root).toString()},
            {QStringLiteral("workspaceFolders"),
             QJsonArray{QJsonObject{{QStringLiteral("uri"), QUrl::fromLocalFile(m_root).toString()},
                                    {QStringLiteral("name"), QFileInfo(m_root).fileName()}}}},
            {QStringLiteral("capabilities"),
             QJsonObject{{QStringLiteral("textDocument"), textDocument},
                         {QStringLiteral("workspace"),
                          QJsonObject{{QStringLiteral("applyEdit"), true},
                                      {QStringLiteral("workspaceEdit"), QJsonObject{{QStringLiteral("documentChanges"), true}}}}},
                         {QStringLiteral("window"), QJsonObject{{QStringLiteral("workDoneProgress"), true}}},
                         {QStringLiteral("general"), QJsonObject{{QStringLiteral("positionEncodings"), QJsonArray{QStringLiteral("utf-16")}}}}}},
        };
        if (!m_initOptions.isEmpty())
            params.insert(QStringLiteral("initializationOptions"), m_initOptions);
        request(QStringLiteral("initialize"), params, [this](const QJsonValue &result, const QJsonObject &error) {
            if (!error.isEmpty()) {
                fail(error.value(QStringLiteral("message")).toString());
                return;
            }
            const QJsonObject info = result.toObject().value(QStringLiteral("serverInfo")).toObject();
            m_serverName = info.value(QStringLiteral("name")).toString();
            m_serverVersion = info.value(QStringLiteral("version")).toString();
            m_capabilities = result.toObject().value(QStringLiteral("capabilities")).toObject();
            notify(QStringLiteral("initialized"), QJsonObject());
            m_state = State::Running;
            emit ready();
        });
    });
}

LspClient::~LspClient()
{
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(300);
    }
}

void LspClient::prependToPath(const QString &dir)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"), dir + QLatin1Char(':') + env.value(QStringLiteral("PATH")));
    m_proc->setProcessEnvironment(env);
}

void LspClient::start()
{
    if (m_state != State::Idle)
        return;
    m_state = State::Starting;
    m_proc->start();
}

void LspClient::shutdown(int waitMs)
{
    if (m_state == State::Stopped || m_state == State::Idle || m_proc->state() == QProcess::NotRunning)
        return;
    m_exiting = true;
    if (m_state == State::Running) {
        request(QStringLiteral("shutdown"), QJsonValue::Null);
        notify(QStringLiteral("exit"), QJsonValue::Undefined);
    }
    m_proc->closeWriteChannel();
    if (!m_proc->waitForFinished(waitMs)) {
        m_proc->kill();
        m_proc->waitForFinished(300);
    }
}

int LspClient::request(const QString &method, const QJsonValue &params, Callback callback)
{
    const int id = m_nextId++;
    if (callback)
        m_pending.insert(id, std::move(callback));
    QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("method"), method}};
    if (!params.isUndefined())
        msg.insert(QStringLiteral("params"), params);
    send(msg);
    return id;
}

void LspClient::notify(const QString &method, const QJsonValue &params)
{
    QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}};
    if (!params.isUndefined())
        msg.insert(QStringLiteral("params"), params);
    send(msg);
}

void LspClient::cancel(int id)
{
    if (m_pending.remove(id))
        notify(QStringLiteral("$/cancelRequest"), QJsonObject{{QStringLiteral("id"), id}});
}

void LspClient::send(const QJsonObject &message)
{
    if (m_proc->state() != QProcess::Running)
        return;
    const QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    m_proc->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
    m_proc->write(body);
}

void LspClient::onStdout()
{
    m_buffer += m_proc->readAllStandardOutput();
    for (;;) {
        const int headerEnd = m_buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        int length = -1;
        const QList<QByteArray> headers = m_buffer.left(headerEnd).split('\n');
        for (const QByteArray &h : headers) {
            const QByteArray line = h.trimmed();
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toInt();
        }
        if (length < 0) { // malformed header: drop it and resynchronise
            m_buffer.remove(0, headerEnd + 4);
            continue;
        }
        if (m_buffer.size() < headerEnd + 4 + length)
            return;
        const QByteArray body = m_buffer.mid(headerEnd + 4, length);
        m_buffer.remove(0, headerEnd + 4 + length);
        handle(body);
    }
}

void LspClient::handle(const QByteArray &body)
{
    const QJsonObject msg = QJsonDocument::fromJson(body).object();
    if (msg.isEmpty())
        return;
    if (msg.contains(QStringLiteral("method"))) {
        if (msg.contains(QStringLiteral("id")))
            handleServerRequest(msg);
        else {
            const QString method = msg.value(QStringLiteral("method")).toString();
            // Some servers (Kotlin) log through the protocol instead of stderr: keep it for Show Server Log.
            if (method == QLatin1String("window/logMessage")) {
                m_log << msg.value(QStringLiteral("params")).toObject().value(QStringLiteral("message")).toString().split(QLatin1Char('\n'));
                while (m_log.size() > kMaxLogLines)
                    m_log.removeFirst();
            }
            emit notification(method, msg.value(QStringLiteral("params")));
        }
        return;
    }
    const int id = msg.value(QStringLiteral("id")).toInt(-1);
    const auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    const Callback cb = it.value();
    m_pending.erase(it);
    cb(msg.value(QStringLiteral("result")), msg.value(QStringLiteral("error")).toObject());
}

// Requests the server sends us. We implement none of the optional ones yet; answer so it never waits.
void LspClient::handleServerRequest(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), message.value(QStringLiteral("id"))}};
    if (method == QLatin1String("workspace/configuration")) {
        QJsonArray nulls;
        const int n = message.value(QStringLiteral("params")).toObject().value(QStringLiteral("items")).toArray().size();
        for (int i = 0; i < n; ++i)
            nulls.append(QJsonValue::Null);
        reply.insert(QStringLiteral("result"), nulls);
    } else if (method == QLatin1String("workspace/applyEdit")) {
        const bool ok = m_applyEdit && m_applyEdit(message.value(QStringLiteral("params")).toObject().value(QStringLiteral("edit")).toObject());
        reply.insert(QStringLiteral("result"), QJsonObject{{QStringLiteral("applied"), ok}});
    } else if (method == QLatin1String("client/registerCapability") || method == QLatin1String("client/unregisterCapability") ||
               method == QLatin1String("window/workDoneProgress/create") || method == QLatin1String("window/showMessageRequest") ||
               method.endsWith(QLatin1String("/refresh"))) {
        reply.insert(QStringLiteral("result"), QJsonValue::Null);
        if (method == QLatin1String("workspace/diagnostic/refresh"))
            QTimer::singleShot(0, this, [this] { emit notification(QStringLiteral("workspace/diagnostic/refresh"), QJsonObject()); });
    } else {
        reply.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601}, {QStringLiteral("message"), QStringLiteral("Method not found")}});
    }
    send(reply);
}

void LspClient::onFinished(int, int exitStatus)
{
    if (m_state == State::Stopped) // fail() already reported it
        return;
    const bool crashed = !m_exiting && m_state != State::Stopped;
    m_state = State::Stopped;
    m_pending.clear();
    if (crashed && m_error.isEmpty())
        m_error = exitStatus == QProcess::CrashExit ? tr("The server crashed") : tr("The server exited unexpectedly");
    emit stopped(crashed);
}

void LspClient::fail(const QString &error)
{
    if (m_state == State::Stopped)
        return;
    m_error = error;
    m_state = State::Stopped;
    m_pending.clear();
    if (m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    emit stopped(true);
}
