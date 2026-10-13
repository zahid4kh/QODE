#include "LspClient.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kMaxLogLines = 200;

// The standard semantic token legend (LSP 3.17): a server only uses the types and modifiers the client lists.
QJsonArray semanticTokenTypes()
{
    QJsonArray a;
    for (const char *t : {"namespace", "type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable", "property",
                          "enumMember", "event", "function", "method", "macro", "keyword", "modifier", "comment", "string", "number",
                          "regexp", "operator", "decorator"})
        a.append(QLatin1String(t));
    return a;
}

QJsonArray semanticTokenModifiers()
{
    QJsonArray a;
    for (const char *t : {"declaration", "definition", "readonly", "static", "deprecated", "abstract", "async", "modification",
                          "documentation", "defaultLibrary"})
        a.append(QLatin1String(t));
    return a;
}
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
            {QStringLiteral("references"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("rename"), QJsonObject{{QStringLiteral("prepareSupport"), true}}},
            {QStringLiteral("colorProvider"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("codeAction"),
             QJsonObject{{QStringLiteral("isPreferredSupport"), true},
                         {QStringLiteral("dataSupport"), true},
                         {QStringLiteral("resolveSupport"), QJsonObject{{QStringLiteral("properties"), QJsonArray{QStringLiteral("edit")}}}}, // edits arrive with codeAction/resolve
                         {QStringLiteral("codeActionLiteralSupport"),
                          QJsonObject{{QStringLiteral("codeActionKind"),
                                       QJsonObject{{QStringLiteral("valueSet"),
                                                    QJsonArray{QStringLiteral("quickfix"), QStringLiteral("refactor"), QStringLiteral("refactor.extract"),
                                                               QStringLiteral("refactor.inline"), QStringLiteral("refactor.move"), QStringLiteral("refactor.rewrite"),
                                                               QStringLiteral("source"), QStringLiteral("source.organizeImports"), QStringLiteral("source.fixAll")}}}}}}}},
            {QStringLiteral("signatureHelp"),
             QJsonObject{{QStringLiteral("contextSupport"), true},
                         {QStringLiteral("signatureInformation"),
                          QJsonObject{{QStringLiteral("documentationFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}},
                                      {QStringLiteral("activeParameterSupport"), true},
                                      {QStringLiteral("parameterInformation"), QJsonObject{{QStringLiteral("labelOffsetSupport"), true}}}}}}},
            {QStringLiteral("documentHighlight"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("typeDefinition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("documentSymbol"), QJsonObject{{QStringLiteral("hierarchicalDocumentSymbolSupport"), true}}},
            {QStringLiteral("foldingRange"), QJsonObject{{QStringLiteral("lineFoldingOnly"), true}, {QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("callHierarchy"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("semanticTokens"),
             QJsonObject{{QStringLiteral("requests"), QJsonObject{{QStringLiteral("range"), false}, {QStringLiteral("full"), true}}},
                         {QStringLiteral("tokenTypes"), semanticTokenTypes()},
                         {QStringLiteral("tokenModifiers"), semanticTokenModifiers()},
                         {QStringLiteral("formats"), QJsonArray{QStringLiteral("relative")}},
                         {QStringLiteral("overlappingTokenSupport"), false},
                         {QStringLiteral("multilineTokenSupport"), false},
                         {QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("inlayHint"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("completion"),
             QJsonObject{{QStringLiteral("contextSupport"), true},
                         {QStringLiteral("completionItem"),
                          QJsonObject{{QStringLiteral("snippetSupport"), true},
                                      {QStringLiteral("deprecatedSupport"), true},
                                      {QStringLiteral("labelDetailsSupport"), true},
                                      {QStringLiteral("documentationFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}},
                                      {QStringLiteral("resolveSupport"), QJsonObject{{QStringLiteral("properties"), QJsonArray{QStringLiteral("documentation"), QStringLiteral("detail")}}}}}}}},
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
                                      {QStringLiteral("configuration"), bool(m_configuration)},
                                      {QStringLiteral("workspaceEdit"), QJsonObject{{QStringLiteral("documentChanges"), true}}},
                                      {QStringLiteral("inlayHint"), QJsonObject{{QStringLiteral("refreshSupport"), true}}},
                                      {QStringLiteral("semanticTokens"), QJsonObject{{QStringLiteral("refreshSupport"), true}}},
                                      {QStringLiteral("foldingRange"), QJsonObject{{QStringLiteral("refreshSupport"), true}}}}},
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

namespace {
bool isInteractive(const QString &m)
{
    return m == QLatin1String("textDocument/completion") || m == QLatin1String("completionItem/resolve") ||
           m == QLatin1String("textDocument/signatureHelp");
}
bool isBackground(const QString &m)
{
    return m == QLatin1String("textDocument/inlayHint") || m == QLatin1String("textDocument/semanticTokens/full") ||
           m == QLatin1String("textDocument/documentSymbol") || m == QLatin1String("textDocument/foldingRange") ||
           m == QLatin1String("textDocument/documentHighlight") || m == QLatin1String("textDocument/diagnostic") ||
           m == QLatin1String("textDocument/documentColor");
}
}

int LspClient::request(const QString &method, const QJsonValue &params, Callback callback)
{
    const int id = m_nextId++;
    if (callback && isBackground(method) && m_interactive > 0) {
        // Same method on the same document supersedes the held one (its caller is told it was cancelled).
        const QString key = method + QLatin1Char('|') + params.toObject().value(QStringLiteral("textDocument")).toObject().value(QStringLiteral("uri")).toString();
        for (int i = 0; i < m_deferred.size(); ++i)
            if (m_deferred[i].key == key) {
                const Callback old = m_deferred[i].callback;
                m_deferred.removeAt(i);
                old(QJsonValue(), QJsonObject{{QStringLiteral("code"), -32800}, {QStringLiteral("message"), QStringLiteral("superseded")}});
                break;
            }
        m_deferred.append({id, method, key, params, std::move(callback)});
        return id;
    }
    transmit(id, method, params, std::move(callback));
    return id;
}

void LspClient::transmit(int id, const QString &method, const QJsonValue &params, Callback callback)
{
    if (callback) {
        if (isInteractive(method)) {
            ++m_interactive;
            m_interactiveIds.insert(id);
            // Hold-back must never outlive a lost answer: flush after 1.5 s whatever happens.
            QTimer::singleShot(1500, this, [this, id] {
                if (m_interactiveIds.remove(id)) {
                    --m_interactive;
                    flushDeferred();
                }
            });
        }
        m_pending.insert(id, std::move(callback));
        Sent s;
        s.method = method;
        s.outstanding = int(m_pending.size()) - 1;
        s.timer.start();
        m_sent.insert(id, s);
    }
    QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("method"), method}};
    if (!params.isUndefined())
        msg.insert(QStringLiteral("params"), params);
    send(msg);
}

void LspClient::flushDeferred()
{
    if (m_interactive > 0 || m_deferred.isEmpty())
        return;
    const QList<Deferred> list = std::exchange(m_deferred, {});
    for (const Deferred &d : list)
        transmit(d.id, d.method, d.params, d.callback);
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
    for (int i = 0; i < m_deferred.size(); ++i)
        if (m_deferred[i].id == id) {
            m_deferred.removeAt(i);
            return;
        }
    m_sent.remove(id);
    if (m_interactiveIds.remove(id))
        --m_interactive;
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
    if (m_parsing) // a handler spun an event loop: the outer call keeps walking the buffer
        return;
    m_parsing = true;
    // Walk the buffer by offset and cut the consumed part once: removing from the front per message is quadratic when
    // an indexing server floods us.
    int pos = 0;
    for (;;) {
        const int headerEnd = m_buffer.indexOf("\r\n\r\n", pos);
        if (headerEnd < 0)
            break;
        int length = -1;
        const QList<QByteArray> headers = m_buffer.mid(pos, headerEnd - pos).split('\n');
        for (const QByteArray &h : headers) {
            const QByteArray line = h.trimmed();
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toInt();
        }
        if (length < 0) { // malformed header: drop it and resynchronise
            pos = headerEnd + 4;
            continue;
        }
        if (m_buffer.size() < headerEnd + 4 + length)
            break;
        const QByteArray body = m_buffer.mid(headerEnd + 4, length);
        pos = headerEnd + 4 + length;
        handle(body);
    }
    if (pos > 0)
        m_buffer.remove(0, pos);
    m_parsing = false;
}

void LspClient::handle(const QByteArray &body)
{
    QElapsedTimer parseTimer;
    parseTimer.start();
    const QJsonObject msg = QJsonDocument::fromJson(body).object();
    const qint64 parseMs = parseTimer.elapsed();
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
    // Timing: how long the server took (round trip) and how long our handler then blocked the UI thread.
    const auto sent = m_sent.take(id);
    const bool wasInteractive = m_interactiveIds.remove(id);
    const qint64 roundTrip = sent.timer.isValid() ? sent.timer.elapsed() : -1;
    QElapsedTimer handler;
    handler.start();
    const QPointer<LspClient> self(this); // a callback may stop or restart the server, deleting us
    cb(msg.value(QStringLiteral("result")), msg.value(QStringLiteral("error")).toObject());
    if (!self)
        return;
    const qint64 ui = handler.elapsed();
    if (wasInteractive) {
        --m_interactive;
        flushDeferred();
        if (!self)
            return;
    }
    if (roundTrip >= 0 && (roundTrip >= 100 || ui + parseMs >= 15 || sent.method.startsWith(QLatin1String("textDocument/completion"))))
        logTiming(QStringLiteral("%1: server %2 ms, handler %3 ms, json parse %6 ms, %4 bytes, %5 other requests were waiting")
                      .arg(sent.method).arg(roundTrip).arg(ui).arg(body.size()).arg(sent.outstanding).arg(parseMs));
}

void LspClient::logTiming(const QString &text)
{
    m_log << QStringLiteral("[qode] ") + text;
    while (m_log.size() > kMaxLogLines)
        m_log.removeFirst();
}

// Requests the server sends us. We implement none of the optional ones yet; answer so it never waits.
void LspClient::handleServerRequest(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), message.value(QStringLiteral("id"))}};
    if (method == QLatin1String("workspace/configuration")) {
        QJsonArray answers;
        for (const QJsonValue &item : message.value(QStringLiteral("params")).toObject().value(QStringLiteral("items")).toArray())
            answers.append(m_configuration ? m_configuration(item.toObject()) : QJsonValue(QJsonValue::Null));
        reply.insert(QStringLiteral("result"), answers);
    } else if (method == QLatin1String("workspace/applyEdit")) {
        const bool ok = m_applyEdit && m_applyEdit(message.value(QStringLiteral("params")).toObject().value(QStringLiteral("edit")).toObject());
        reply.insert(QStringLiteral("result"), QJsonObject{{QStringLiteral("applied"), ok}});
    } else if (method.startsWith(QLatin1String("eslint/"))) {
        // ESLint asks things like "may I run this project's ESLint?" (4 = always allowed) and "no library found" (ignored).
        if (method == QLatin1String("eslint/openDoc")) // "Show documentation for <rule>"
            QDesktopServices::openUrl(QUrl(message.value(QStringLiteral("params")).toObject().value(QStringLiteral("url")).toString()));
        reply.insert(QStringLiteral("result"), method == QLatin1String("eslint/confirmESLintExecution") ? QJsonValue(4) : QJsonValue(QJsonValue::Null));
    } else if (method == QLatin1String("_typescript.rename")) {
        // After "Extract to function" the TypeScript server asks the editor to start a rename at the new name.
        reply.insert(QStringLiteral("result"), QJsonValue(QJsonValue::Null));
        const QJsonValue params = message.value(QStringLiteral("params"));
        QTimer::singleShot(0, this, [this, params] { emit notification(QStringLiteral("_typescript.rename"), params); });
    } else if (method == QLatin1String("client/registerCapability") || method == QLatin1String("client/unregisterCapability") ||
               method == QLatin1String("window/workDoneProgress/create") || method == QLatin1String("window/showMessageRequest") ||
               method.endsWith(QLatin1String("/refresh"))) {
        reply.insert(QStringLiteral("result"), QJsonValue::Null);
        if (method == QLatin1String("workspace/diagnostic/refresh") || method == QLatin1String("workspace/inlayHint/refresh") ||
            method == QLatin1String("workspace/semanticTokens/refresh") || method == QLatin1String("workspace/foldingRange/refresh"))
            QTimer::singleShot(0, this, [this, method] { emit notification(method, QJsonObject()); });
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
    m_sent.clear();
    m_deferred.clear();
    m_interactiveIds.clear();
    m_interactive = 0;
    if (crashed && m_error.isEmpty()) {
        // Kotlin's EAP builds print "This build of kotlin-server has expired." on stdout and quit.
        const QString out = QString::fromUtf8(m_buffer + m_proc->readAllStandardOutput());
        if (out.contains(QLatin1String("expired")))
            m_error = tr("This build of the server has expired — choose Update / Reinstall to get a newer one");
    }
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
    m_sent.clear();
    m_deferred.clear();
    m_interactiveIds.clear();
    m_interactive = 0;
    if (m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    emit stopped(true);
}
