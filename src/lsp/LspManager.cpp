#include "LspManager.h"

#include "JarSource.h"
#include "LspClient.h"
#include "LspServers.h"
#include "editor/Document.h"
#include "project/QmakeProject.h"
#include "settings/SettingsManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kMaxRestarts = 2;
constexpr int kChangeDelayMs = 200;
QString uriFor(const QString &path)
{
    const QString jar = JarSource::uriForPath(path); // unpacked library source: the server knows it by its jar URI
    return jar.isEmpty() ? QUrl::fromLocalFile(path).toString() : jar;
}
} // namespace

LspManager::LspManager(QObject *parent) : QObject(parent), m_changeTimer(new QTimer(this))
{
    m_changeTimer->setSingleShot(true);
    m_changeTimer->setInterval(kChangeDelayMs);
    connect(m_changeTimer, &QTimer::timeout, this, &LspManager::flushChanges);
}

LspManager::~LspManager()
{
    shutdown();
}

// --- Lifecycle --------------------------------------------------------------------------------

void LspManager::setProjectRoot(const QString &root)
{
    if (root == m_root)
        return;
    shutdown();
    m_root = root;
}

void LspManager::shutdown()
{
    for (auto it = m_servers.begin(); it != m_servers.end(); ++it)
        stopServer(it.value());
    m_servers.clear();
    const QList<QString> paths = m_diagnostics.keys();
    m_diagnostics.clear();
    for (const QString &p : paths)
        emit diagnosticsChanged(p);
    for (Tracked &t : m_tracked)
        t.opened = false;
    emit statusChanged();
}

void LspManager::stopServer(Server &s)
{
    if (!s.client)
        return;
    s.client->disconnect(this);
    s.client->shutdown();
    delete s.client;
    s.client = nullptr;
}

QString LspManager::rootFor(const QString &filePath) const
{
    return m_root.isEmpty() ? QFileInfo(filePath).absolutePath() : m_root;
}

LspManager::Server *LspManager::ensureServer(const LspServerSpec &spec, const QString &filePath)
{
    Server &s = m_servers[spec.id];
    if (s.client)
        return &s;
    if (!s.spec) {
        s.spec = &spec;
        s.state.id = spec.id;
        s.state.name = spec.displayName;
    }
    // Missing / crashed servers are retried only through restart() (LSP menu), not on every opened file.
    if (s.state.status == Status::NotFound || s.state.status == Status::Crashed)
        return &s;
    startServer(s, rootFor(filePath));
    return &s;
}

void LspManager::startServer(Server &s, const QString &rootPath)
{
    const QString exe = LspServers::locate(*s.spec, serverPath(s.spec->id));
    s.state.path = exe;
    if (exe.isEmpty()) {
        const QString configured = serverPath(s.spec->id);
        setStatus(s, Status::NotFound,
                  configured.isEmpty() ? tr("not found on PATH") : tr("%1 is not an executable file").arg(configured));
        return;
    }
    QJsonObject options;
    if (s.spec->fallbackFlags) {
        QStringList flags;
        if (!LspServers::hasCompileDatabase(rootPath)) {
            const QmakeProject qmake = QmakeProject::detect(rootPath);
            flags = qmake.compilerFlags();
        }
        flags += parseFlags(projectFlagsText(), rootPath); // the user's lines come last and win
        if (!flags.isEmpty())
            options.insert(QStringLiteral("fallbackFlags"), QJsonArray::fromStringList(flags));
    }
    // {cache}: a folder per server and project for the server's own indexes (survives restarts, never in the project).
    QStringList args = s.spec->arguments;
    if (args.join(QLatin1Char(' ')).contains(QLatin1String("{cache}"))) {
        const QString key = QString::fromLatin1(QCryptographicHash::hash(rootPath.toUtf8(), QCryptographicHash::Md5).toHex().left(12));
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE/lsp/") +
                              s.spec->id + QLatin1Char('/') + key;
        QDir().mkpath(cache);
        args.replaceInStrings(QStringLiteral("{cache}"), cache);
    }
    s.progress.clear();
    s.state.progress.clear();
    s.client = new LspClient(exe, args, rootPath, options, this);
    s.client->setApplyEditHandler(m_applyEdit);
    const QString id = s.spec->id;
    connect(s.client, &LspClient::ready, this, [this, id] { onServerReady(id); });
    connect(s.client, &LspClient::stopped, this, [this, id](bool crashed) { onServerStopped(id, crashed); });
    connect(s.client, &LspClient::notification, this,
            [this, id](const QString &m, const QJsonValue &p) { onNotification(id, m, p); });
    setStatus(s, Status::Starting);
    s.client->start();
}

void LspManager::onServerReady(const QString &id)
{
    Server &s = m_servers[id];
    QString detail = s.client->serverName();
    if (!s.client->serverVersion().isEmpty())
        detail += QLatin1Char(' ') + s.client->serverVersion();
    setStatus(s, Status::Running, detail);
    for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it)
        if (it->serverId == id && !it->opened)
            sendOpen(it.key(), it.value());
}

void LspManager::onServerStopped(const QString &id, bool crashed)
{
    Server &s = m_servers[id];
    const QString error = s.client ? s.client->errorString() : QString();
    if (s.client) {
        s.client->deleteLater();
        s.client = nullptr;
    }
    s.progress.clear();
    s.state.progress.clear();
    for (Tracked &t : m_tracked)
        if (t.serverId == id)
            t.opened = false;
    for (const Tracked &t : std::as_const(m_tracked))
        if (t.serverId == id)
            clearDiagnosticsFor(t.path);
    if (!crashed) {
        setStatus(s, Status::Idle);
        return;
    }
    if (s.restarts < kMaxRestarts) {
        ++s.restarts;
        setStatus(s, Status::Starting, tr("restarting after a crash"));
        QTimer::singleShot(800, this, [this, id] {
            Server &srv = m_servers[id];
            if (srv.client || !srv.spec)
                return;
            QString root = m_root;
            if (root.isEmpty())
                for (const Tracked &t : std::as_const(m_tracked))
                    if (t.serverId == id) {
                        root = QFileInfo(t.path).absolutePath();
                        break;
                    }
            startServer(srv, root);
        });
        return;
    }
    setStatus(s, Status::Crashed, error);
}

void LspManager::restart(const QString &serverId)
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    if (!spec)
        return;
    Server &s = m_servers[serverId];
    stopServer(s);
    s.spec = spec;
    s.state.id = spec->id;
    s.state.name = spec->displayName;
    s.restarts = 0;
    for (Tracked &t : m_tracked)
        if (t.serverId == serverId)
            t.opened = false;
    QString root = m_root;
    if (root.isEmpty())
        for (const Tracked &t : std::as_const(m_tracked))
            if (t.serverId == serverId) {
                root = QFileInfo(t.path).absolutePath();
                break;
            }
    // Only start when a document needs it; otherwise wait for the next one.
    bool needed = false;
    for (const Tracked &t : std::as_const(m_tracked))
        needed = needed || t.serverId == serverId;
    if (needed) {
        startServer(s, root);
    } else {
        s.state.path = LspServers::locate(*spec, serverPath(serverId));
        setStatus(s, s.state.path.isEmpty() ? Status::NotFound : Status::Idle,
                  s.state.path.isEmpty() ? tr("not found on PATH") : QString());
    }
}

// --- Settings / state ----------------------------------------------------------------------------

QString LspManager::serverPath(const QString &serverId) const
{
    return SettingsManager::instance().lspServerPath(serverId);
}

void LspManager::setServerPath(const QString &serverId, const QString &path)
{
    SettingsManager::instance().setLspServerPath(serverId, path);
    restart(serverId);
}

QString LspManager::installHelp(const QString &serverId) const
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    return spec ? spec->installHelp : QString();
}

QList<LspManager::ServerState> LspManager::servers() const
{
    QList<ServerState> out;
    for (const LspServerSpec &spec : LspServers::all()) {
        const auto it = m_servers.constFind(spec.id);
        ServerState st;
        if (it != m_servers.constEnd()) {
            st = it->state;
        } else {
            st.id = spec.id;
            st.name = spec.displayName;
            // Show "not installed" up front, before any C++ file is opened.
            st.path = LspServers::locate(spec, serverPath(spec.id));
            if (st.path.isEmpty())
                st.status = Status::NotFound;
        }
        st.installable = spec.installable;
        for (const Tracked &t : m_tracked)
            st.documents += t.serverId == spec.id;
        out << st;
    }
    return out;
}

QStringList LspManager::logOf(const QString &serverId) const
{
    const auto it = m_servers.constFind(serverId);
    return it != m_servers.constEnd() && it->client ? it->client->logLines() : QStringList();
}

QStringList LspManager::detectedFlags(QString *source) const
{
    if (m_root.isEmpty() || LspServers::hasCompileDatabase(m_root))
        return {};
    const QmakeProject qmake = QmakeProject::detect(m_root);
    if (!qmake.isValid())
        return {};
    if (source)
        *source = qmake.proFileName();
    return qmake.compilerFlags();
}

QString LspManager::projectFlagsText() const
{
    return SettingsManager::instance().lspFlagsText();
}

void LspManager::setProjectFlagsText(const QString &text)
{
    SettingsManager::instance().setLspFlagsText(text);
    for (const LspServerSpec &spec : LspServers::all())
        if (spec.fallbackFlags && m_servers.contains(spec.id))
            restart(spec.id);
}

QStringList LspManager::parseFlags(const QString &text, const QString &projectRoot)
{
    QStringList flags;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        line.replace(QStringLiteral("{project}"), projectRoot);
        flags << QProcess::splitCommand(line);
    }
    return flags;
}

bool LspManager::hasCompileDatabase() const
{
    return LspServers::hasCompileDatabase(m_root);
}

int LspManager::diagnosticCount(int severity) const
{
    int n = 0;
    for (const auto &list : m_diagnostics)
        for (const LspDiagnostic &d : list)
            n += d.severity == severity;
    return n;
}

void LspManager::setStatus(Server &s, Status status, const QString &detail)
{
    s.state.status = status;
    s.state.detail = detail;
    emit statusChanged();
}

// --- Document sync ------------------------------------------------------------------------------

void LspManager::documentOpened(Document *doc)
{
    if (!doc || doc->isUntitled() || m_tracked.contains(doc))
        return;
    track(doc);
}

void LspManager::track(Document *doc)
{
    const LspServerSpec *spec = LspServers::forFile(doc->filePath());
    if (!spec)
        return;
    if (JarSource::isLibraryPath(doc->filePath()) && JarSource::uriForPath(doc->filePath()).isEmpty())
        return; // unpacked in an earlier session: the server cannot map it back to its jar
    Tracked t;
    t.path = doc->filePath();
    t.uri = uriFor(t.path);
    t.serverId = spec->id;
    m_tracked.insert(doc, t);
    connect(doc->textDocument(), &QTextDocument::contentsChanged, this, [this, doc] {
        auto it = m_tracked.find(doc);
        if (it == m_tracked.end())
            return;
        it->dirty = true;
        m_changeTimer->start();
    });
    connect(doc, &QObject::destroyed, this, [this, doc] {
        auto it = m_tracked.find(doc);
        if (it == m_tracked.end())
            return;
        const Tracked t = it.value();
        m_tracked.erase(it);
        Server &s = m_servers[t.serverId];
        if (t.opened && s.client && s.client->isRunning())
            s.client->notify(QStringLiteral("textDocument/didClose"),
                             QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), t.uri}}}});
        clearDiagnosticsFor(t.path);
    });

    Server *s = ensureServer(*spec, t.path);
    if (s && s->client && s->client->isRunning())
        sendOpen(doc, m_tracked[doc]);
    else
        emit statusChanged();
}

void LspManager::untrack(Document *doc, bool sendClose)
{
    auto it = m_tracked.find(doc);
    if (it == m_tracked.end())
        return;
    const Tracked t = it.value();
    m_tracked.erase(it);
    doc->textDocument()->disconnect(this);
    disconnect(doc, &QObject::destroyed, this, nullptr);
    Server &s = m_servers[t.serverId];
    if (sendClose && t.opened && s.client && s.client->isRunning())
        s.client->notify(QStringLiteral("textDocument/didClose"),
                         QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), t.uri}}}});
    clearDiagnosticsFor(t.path);
}

void LspManager::sendOpen(Document *doc, Tracked &t)
{
    Server &s = m_servers[t.serverId];
    if (!s.client || !s.client->isRunning())
        return;
    t.version = 1;
    t.opened = true;
    t.dirty = false;
    s.client->notify(QStringLiteral("textDocument/didOpen"),
                     QJsonObject{{QStringLiteral("textDocument"),
                                  QJsonObject{{QStringLiteral("uri"), t.uri},
                                              {QStringLiteral("languageId"), s.spec->languageId(t.path)},
                                              {QStringLiteral("version"), t.version},
                                              {QStringLiteral("text"), doc->text()}}}});
    pullDiagnostics(doc);
}

void LspManager::flushChanges()
{
    for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it) {
        Tracked &t = it.value();
        if (!t.dirty || !t.opened)
            continue;
        Server &s = m_servers[t.serverId];
        if (!s.client || !s.client->isRunning())
            continue;
        t.dirty = false;
        ++t.version;
        s.client->notify(QStringLiteral("textDocument/didChange"),
                         QJsonObject{{QStringLiteral("textDocument"),
                                      QJsonObject{{QStringLiteral("uri"), t.uri}, {QStringLiteral("version"), t.version}}},
                                     {QStringLiteral("contentChanges"),
                                      QJsonArray{QJsonObject{{QStringLiteral("text"), it.key()->text()}}}}});
        pullDiagnostics(it.key());
    }
}

void LspManager::documentSaved(Document *doc)
{
    auto it = m_tracked.find(doc);
    if (it == m_tracked.end() || !it->opened)
        return;
    flushChanges();
    Server &s = m_servers[it->serverId];
    if (s.client && s.client->isRunning())
        s.client->notify(QStringLiteral("textDocument/didSave"),
                         QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), it->uri}}}});
    pullDiagnostics(doc);
}

// A qmake .pro / .pri changed: the flags derived from it are stale, so servers using them restart.
void LspManager::buildFileSaved(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    if (suffix != QLatin1String("pro") && suffix != QLatin1String("pri"))
        return;
    for (const LspServerSpec &spec : LspServers::all())
        if (spec.fallbackFlags && m_servers.contains(spec.id) && m_servers[spec.id].client)
            restart(spec.id);
}

// Save As / rename: the server knows the file under its old URI, so close it and open it again.
void LspManager::documentPathChanged(Document *doc)
{
    const auto it = m_tracked.constFind(doc);
    if (it != m_tracked.constEnd() && it->uri == uriFor(doc->filePath()))
        return;
    untrack(doc, true);
    documentOpened(doc);
}

// --- Requests --------------------------------------------------------------------------------------

LspClient *LspManager::readyClientFor(Document *doc, QString *uri)
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened)
        return nullptr;
    const Server &s = m_servers[it->serverId];
    if (!s.client || !s.client->isRunning() || s.state.status != Status::Running)
        return nullptr;
    flushChanges(); // the server must see the text the cursor position refers to
    *uri = it->uri;
    return s.client;
}

bool LspManager::isServed(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    return it != m_tracked.constEnd() && it->opened;
}

namespace {
QJsonObject positionParams(const QString &uri, int line, int column)
{
    return {{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}},
            {QStringLiteral("position"), QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), column}}}};
}

// MarkupContent | MarkedString | MarkedString[] -> text
QString markupText(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue &e : v.toArray()) {
            const QString t = markupText(e);
            if (!t.isEmpty())
                parts << t;
        }
        return parts.join(QStringLiteral("\n\n"));
    }
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("language"))) // MarkedString { language, value }
        return QStringLiteral("```%1\n%2\n```").arg(o.value(QStringLiteral("language")).toString(), o.value(QStringLiteral("value")).toString());
    return o.value(QStringLiteral("value")).toString();
}

void addLocation(QVector<LspLocation> &out, const QJsonObject &o)
{
    // Location { uri, range } or LocationLink { targetUri, targetSelectionRange }
    const bool link = o.contains(QStringLiteral("targetUri"));
    const QString uri = o.value(link ? QStringLiteral("targetUri") : QStringLiteral("uri")).toString();
    const QJsonObject range = o.value(link ? QStringLiteral("targetSelectionRange") : QStringLiteral("range")).toObject();
    const QJsonObject start = range.value(QStringLiteral("start")).toObject();
    LspLocation loc;
    loc.path = JarSource::isJarUri(uri) ? JarSource::extract(uri) : QUrl(uri).toLocalFile();
    loc.line = start.value(QStringLiteral("line")).toInt();
    loc.column = start.value(QStringLiteral("character")).toInt();
    if (!loc.path.isEmpty())
        out << loc;
}

LspTextEdit parseEdit(const QJsonObject &o, const QString &rangeKey = QStringLiteral("range"))
{
    const QJsonObject range = o.value(rangeKey).toObject();
    const QJsonObject a = range.value(QStringLiteral("start")).toObject();
    const QJsonObject b = range.value(QStringLiteral("end")).toObject();
    LspTextEdit e;
    e.startLine = a.value(QStringLiteral("line")).toInt();
    e.startColumn = a.value(QStringLiteral("character")).toInt();
    e.endLine = b.value(QStringLiteral("line")).toInt();
    e.endColumn = b.value(QStringLiteral("character")).toInt();
    e.text = o.value(QStringLiteral("newText")).toString();
    return e;
}

LspCompletionItem parseCompletionItem(const QJsonObject &o)
{
    LspCompletionItem it;
    it.label = o.value(QStringLiteral("label")).toString();
    it.kind = o.value(QStringLiteral("kind")).toInt();
    it.detail = o.value(QStringLiteral("detail")).toString();
    const QJsonObject details = o.value(QStringLiteral("labelDetails")).toObject();
    it.signature = details.value(QStringLiteral("detail")).toString();
    if (it.detail.isEmpty())
        it.detail = details.value(QStringLiteral("description")).toString();
    it.insertText = o.value(QStringLiteral("insertText")).toString();
    it.filterText = o.value(QStringLiteral("filterText")).toString();
    it.sortText = o.value(QStringLiteral("sortText")).toString();
    it.snippet = o.value(QStringLiteral("insertTextFormat")).toInt() == 2;
    it.deprecated = o.value(QStringLiteral("deprecated")).toBool() ||
                    o.value(QStringLiteral("tags")).toArray().contains(1);
    const QJsonObject te = o.value(QStringLiteral("textEdit")).toObject();
    if (!te.isEmpty()) {
        // TextEdit { range, newText } or InsertReplaceEdit { insert, replace, newText }
        it.edit = parseEdit(te, te.contains(QStringLiteral("replace")) ? QStringLiteral("replace") : QStringLiteral("range"));
        it.hasEdit = true;
    }
    for (const QJsonValue &v : o.value(QStringLiteral("additionalTextEdits")).toArray())
        it.additionalEdits << parseEdit(v.toObject());
    const QJsonObject command = o.value(QStringLiteral("command")).toObject();
    it.command = command.value(QStringLiteral("command")).toString();
    it.commandArgs = command.value(QStringLiteral("arguments")).toArray();
    return it;
}
} // namespace

void LspManager::completion(Document *doc, int line, int column, int triggerKind, const QString &triggerChar,
                            std::function<void(const QVector<LspCompletionItem> &, bool)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({}, false);
        return;
    }
    if (m_completionClient && m_completionId >= 0)
        m_completionClient->cancel(m_completionId);
    QJsonObject context{{QStringLiteral("triggerKind"), triggerKind}};
    if (triggerKind == 2 && !triggerChar.isEmpty())
        context.insert(QStringLiteral("triggerCharacter"), triggerChar);
    QJsonObject params = positionParams(uri, line, column);
    params.insert(QStringLiteral("context"), context);
    m_completionClient = c;
    m_completionId = c->request(QStringLiteral("textDocument/completion"), params,
                                [done](const QJsonValue &result, const QJsonObject &error) {
                                    QVector<LspCompletionItem> out;
                                    bool incomplete = false;
                                    if (error.isEmpty()) {
                                        QJsonArray items;
                                        if (result.isArray()) {
                                            items = result.toArray();
                                        } else if (result.isObject()) {
                                            incomplete = result.toObject().value(QStringLiteral("isIncomplete")).toBool();
                                            items = result.toObject().value(QStringLiteral("items")).toArray();
                                        }
                                        out.reserve(items.size());
                                        for (const QJsonValue &v : items)
                                            out << parseCompletionItem(v.toObject());
                                    }
                                    done(out, incomplete);
                                });
}

QHash<QString, QVector<LspTextEdit>> LspManager::editsOf(const QJsonObject &edit, bool *ok)
{
    QHash<QString, QVector<LspTextEdit>> out;
    *ok = true;
    auto add = [&out](const QString &uri, const QJsonArray &edits) {
        const QString path = QUrl(uri).toLocalFile();
        if (path.isEmpty())
            return false;
        for (const QJsonValue &e : edits)
            out[path] << parseEdit(e.toObject());
        return true;
    };
    const QJsonObject changes = edit.value(QStringLiteral("changes")).toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        *ok = add(it.key(), it.value().toArray()) && *ok;
    for (const QJsonValue &v : edit.value(QStringLiteral("documentChanges")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.contains(QStringLiteral("kind"))) { // create / rename / delete file: not supported
            *ok = false;
            continue;
        }
        *ok = add(o.value(QStringLiteral("textDocument")).toObject().value(QStringLiteral("uri")).toString(), o.value(QStringLiteral("edits")).toArray()) && *ok;
    }
    return out;
}

bool LspManager::supportsCommand(Document *doc, const QString &command) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || command.isEmpty())
        return false;
    const LspClient *client = m_servers.value(it->serverId).client;
    if (!client || !client->isRunning())
        return false;
    const QJsonArray commands =
        client->serverCapabilities().value(QStringLiteral("executeCommandProvider")).toObject().value(QStringLiteral("commands")).toArray();
    return commands.contains(command);
}

// The server computes the text of a Kotlin completion (and its import) when the command runs, relative to the
// session of the request that produced the item. The popup filters locally, so the document has usually moved
// on since: ask again at the current position, pick the same item and run its command, so server and editor agree.
void LspManager::runCompletionCommand(Document *doc, const LspCompletionItem &item, int line, int column, std::function<void(bool)> done)
{
    QString uri;
    LspClient *c = supportsCommand(doc, item.command) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done(false);
        return;
    }
    QJsonObject params = positionParams(uri, line, column);
    params.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("triggerKind"), 1}});
    QPointer<LspClient> guard(c);
    c->request(QStringLiteral("textDocument/completion"), params, [guard, item, done](const QJsonValue &result, const QJsonObject &error) {
        if (!guard || !error.isEmpty())
            return done(false);
        const QJsonArray items = result.isArray() ? result.toArray() : result.toObject().value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const LspCompletionItem fresh = parseCompletionItem(v.toObject());
            if (fresh.label != item.label || fresh.signature != item.signature || fresh.command.isEmpty())
                continue;
            guard->request(QStringLiteral("workspace/executeCommand"),
                           QJsonObject{{QStringLiteral("command"), fresh.command}, {QStringLiteral("arguments"), fresh.commandArgs}},
                           [done](const QJsonValue &, const QJsonObject &err) { done(err.isEmpty()); });
            return;
        }
        done(false);
    });
}

void LspManager::codeActions(Document *doc, int startLine, int startColumn, int endLine, int endColumn,
                             std::function<void(const QVector<LspCodeAction> &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    QJsonArray diagnostics;
    for (const LspDiagnostic &d : m_diagnostics.value(doc->filePath())) {
        const bool before = d.endLine < startLine || (d.endLine == startLine && d.endColumn < startColumn);
        const bool after = d.startLine > endLine || (d.startLine == endLine && d.startColumn > endColumn);
        if (!before && !after && !d.raw.isEmpty())
            diagnostics.append(d.raw);
    }
    const QJsonObject range{{QStringLiteral("start"), QJsonObject{{QStringLiteral("line"), startLine}, {QStringLiteral("character"), startColumn}}},
                            {QStringLiteral("end"), QJsonObject{{QStringLiteral("line"), endLine}, {QStringLiteral("character"), endColumn}}}};
    c->request(QStringLiteral("textDocument/codeAction"),
               QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}},
                           {QStringLiteral("range"), range},
                           {QStringLiteral("context"), QJsonObject{{QStringLiteral("diagnostics"), diagnostics}}}},
               [done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspCodeAction> out;
                   if (error.isEmpty())
                       for (const QJsonValue &v : result.toArray()) {
                           const QJsonObject o = v.toObject();
                           // A bare Command has `command` as a string; a CodeAction has a title and optional edit/command.
                           if (o.value(QStringLiteral("title")).toString().isEmpty() || o.value(QStringLiteral("disabled")).isObject())
                               continue;
                           out.append({o.value(QStringLiteral("title")).toString(), o.value(QStringLiteral("kind")).toString(), o});
                       }
                   done(out);
               });
}

void LspManager::runCodeAction(Document *doc, const LspCodeAction &action, std::function<void(bool)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri); // also flushes pending edits
    if (!c) {
        done(false);
        return;
    }
    bool ok = true;
    const QJsonObject edit = action.json.value(QStringLiteral("edit")).toObject();
    if (!edit.isEmpty())
        ok = m_applyEdit && m_applyEdit(edit);
    const QJsonValue command = action.json.value(QStringLiteral("command"));
    // CodeAction.command is a Command object; a bare Command (the whole action) carries a string.
    const QJsonObject cmd = command.isObject() ? command.toObject() : QJsonObject();
    if (cmd.isEmpty()) {
        done(ok);
        return;
    }
    c->request(QStringLiteral("workspace/executeCommand"),
               QJsonObject{{QStringLiteral("command"), cmd.value(QStringLiteral("command"))}, {QStringLiteral("arguments"), cmd.value(QStringLiteral("arguments"))}},
               [done, ok](const QJsonValue &, const QJsonObject &err) { done(ok && err.isEmpty()); });
}

void LspManager::hover(Document *doc, int line, int column, std::function<void(const QString &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    c->request(QStringLiteral("textDocument/hover"), positionParams(uri, line, column),
               [done](const QJsonValue &result, const QJsonObject &error) {
                   if (!error.isEmpty() || !result.isObject())
                       return done({});
                   done(markupText(result.toObject().value(QStringLiteral("contents"))).trimmed());
               });
}

void LspManager::definition(Document *doc, int line, int column, std::function<void(const QVector<LspLocation> &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    c->request(QStringLiteral("textDocument/definition"), positionParams(uri, line, column),
               [done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspLocation> out;
                   if (error.isEmpty()) {
                       if (result.isArray())
                           for (const QJsonValue &v : result.toArray())
                               addLocation(out, v.toObject());
                       else if (result.isObject())
                           addLocation(out, result.toObject());
                   }
                   done(out);
               });
}

// --- Server -> client ------------------------------------------------------------------------------

void LspManager::onNotification(const QString &id, const QString &method, const QJsonValue &params)
{
    if (method == QLatin1String("$/progress")) {
        // Work-done progress: "begin" / "report" / "end" for a token. Show the newest running operation.
        Server &s = m_servers[id];
        const QJsonObject o = params.toObject();
        const QString token = o.value(QStringLiteral("token")).toVariant().toString();
        const QJsonObject v = o.value(QStringLiteral("value")).toObject();
        const QString kind = v.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("end")) {
            s.progress.remove(token);
        } else {
            QString title = v.value(QStringLiteral("title")).toString();
            if (title.isEmpty())
                title = s.progress.value(token).value(0); // "report" messages omit it
            QString text = title;
            const QString message = v.value(QStringLiteral("message")).toString();
            if (!message.isEmpty())
                text += QStringLiteral(" — ") + message;
            if (v.contains(QStringLiteral("percentage")))
                text += QStringLiteral(" %1%").arg(v.value(QStringLiteral("percentage")).toInt());
            s.progress.insert(token, {title, text});
            s.state.progress = text;
        }
        if (s.progress.isEmpty())
            s.state.progress.clear();
        else if (kind == QLatin1String("end"))
            s.state.progress = s.progress.constBegin()->value(1);
        emit statusChanged();
        return;
    }
    if (method == QLatin1String("workspace/diagnostic/refresh")) {
        for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it)
            if (it->serverId == id && it->opened)
                pullDiagnostics(it.key());
        return;
    }
    if (method != QLatin1String("textDocument/publishDiagnostics"))
        return;
    const QJsonObject o = params.toObject();
    setDiagnosticsFor(QUrl(o.value(QStringLiteral("uri")).toString()).toLocalFile(), o.value(QStringLiteral("diagnostics")).toArray());
}

void LspManager::setDiagnosticsFor(const QString &path, const QJsonArray &items)
{
    if (path.isEmpty())
        return;
    QVector<LspDiagnostic> list;
    for (const QJsonValue &v : items) {
        const QJsonObject d = v.toObject();
        const QJsonObject range = d.value(QStringLiteral("range")).toObject();
        const QJsonObject a = range.value(QStringLiteral("start")).toObject();
        const QJsonObject b = range.value(QStringLiteral("end")).toObject();
        LspDiagnostic diag;
        diag.startLine = a.value(QStringLiteral("line")).toInt();
        diag.startColumn = a.value(QStringLiteral("character")).toInt();
        diag.endLine = b.value(QStringLiteral("line")).toInt();
        diag.endColumn = b.value(QStringLiteral("character")).toInt();
        diag.severity = d.value(QStringLiteral("severity")).toInt(LspDiagnostic::Error);
        diag.message = d.value(QStringLiteral("message")).toString();
        diag.source = d.value(QStringLiteral("source")).toString();
        diag.code = d.value(QStringLiteral("code")).toVariant().toString();
        diag.raw = d;
        list.append(diag);
    }
    if (list.isEmpty())
        m_diagnostics.remove(path);
    else
        m_diagnostics.insert(path, list);
    emit diagnosticsChanged(path);
    emit statusChanged();
}

// Pull model: the server publishes nothing on its own, so ask after every open / change / save.
void LspManager::pullDiagnostics(Document *doc)
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened)
        return;
    LspClient *client = m_servers.value(it->serverId).client;
    if (!client || !client->isRunning() || !client->serverCapabilities().contains(QStringLiteral("diagnosticProvider")))
        return;
    const QString path = it->path;
    QPointer<LspClient> guard(client);
    client->request(QStringLiteral("textDocument/diagnostic"),
                    QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), it->uri}}}},
                    [this, path, guard](const QJsonValue &result, const QJsonObject &error) {
                        if (!guard || !error.isEmpty())
                            return;
                        const QJsonObject r = result.toObject();
                        if (r.value(QStringLiteral("kind")).toString() == QLatin1String("full"))
                            setDiagnosticsFor(path, r.value(QStringLiteral("items")).toArray());
                    });
}

void LspManager::clearDiagnosticsFor(const QString &path)
{
    if (m_diagnostics.remove(path) > 0) {
        emit diagnosticsChanged(path);
        emit statusChanged();
    }
}
