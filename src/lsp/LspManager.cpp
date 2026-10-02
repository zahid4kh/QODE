#include "LspManager.h"

#include "LspClient.h"
#include "LspServers.h"
#include "editor/Document.h"
#include "project/QmakeProject.h"
#include "settings/SettingsManager.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kMaxRestarts = 2;
constexpr int kChangeDelayMs = 200;
QString uriFor(const QString &path) { return QUrl::fromLocalFile(path).toString(); }
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
    s.client = new LspClient(exe, s.spec->arguments, rootPath, options, this);
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
    if (needed)
        startServer(s, root);
    else
        setStatus(s, Status::Idle);
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
    loc.path = QUrl(uri).toLocalFile();
    loc.line = start.value(QStringLiteral("line")).toInt();
    loc.column = start.value(QStringLiteral("character")).toInt();
    if (!loc.path.isEmpty())
        out << loc;
}
} // namespace

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

void LspManager::onNotification(const QString &, const QString &method, const QJsonValue &params)
{
    if (method != QLatin1String("textDocument/publishDiagnostics"))
        return;
    const QJsonObject o = params.toObject();
    const QString path = QUrl(o.value(QStringLiteral("uri")).toString()).toLocalFile();
    if (path.isEmpty())
        return;
    QVector<LspDiagnostic> list;
    for (const QJsonValue &v : o.value(QStringLiteral("diagnostics")).toArray()) {
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
        list.append(diag);
    }
    if (list.isEmpty())
        m_diagnostics.remove(path);
    else
        m_diagnostics.insert(path, list);
    emit diagnosticsChanged(path);
    emit statusChanged();
}

void LspManager::clearDiagnosticsFor(const QString &path)
{
    if (m_diagnostics.remove(path) > 0) {
        emit diagnosticsChanged(path);
        emit statusChanged();
    }
}
