#include "LspManager.h"

#include "JarSource.h"
#include "LspClient.h"
#include "LspServers.h"
#include "LspSync.h"
#include "editor/Document.h"
#include "project/PythonEnv.h"
#include "project/QmakeProject.h"
#include "settings/SettingsManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace {
constexpr int kMaxRestarts = 2;
constexpr int kChangeDelayMs = 200;
// Settings for servers that ask workspace/configuration (ESLint, Tailwind CSS). Everything else gets null = defaults.
QJsonValue configurationFor(const QString &serverId, const QJsonObject &item, const QString &root)
{
    const QString section = item.value(QStringLiteral("section")).toString();
    if (serverId == QLatin1String("eslint")) {
        const QString scope = item.value(QStringLiteral("scopeUri")).toString();
        const QString file = QUrl(scope).toLocalFile();
        // Run ESLint from the project root (where its config and node_modules live).
        return QJsonObject{
            {QStringLiteral("validate"), QStringLiteral("on")},
            {QStringLiteral("packageManager"), QStringLiteral("npm")},
            {QStringLiteral("useESLintClass"), false},
            {QStringLiteral("experimental"), QJsonObject()}, // the server reads experimental.useFlatConfig: it must exist
            {QStringLiteral("codeActionOnSave"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("all")}}},
            {QStringLiteral("format"), false},
            {QStringLiteral("quiet"), false},
            {QStringLiteral("onIgnoredFiles"), QStringLiteral("off")},
            {QStringLiteral("options"), QJsonObject()},
            {QStringLiteral("rulesCustomizations"), QJsonArray()},
            {QStringLiteral("run"), QStringLiteral("onType")},
            {QStringLiteral("nodePath"), QJsonValue::Null},
            {QStringLiteral("workspaceFolder"), QJsonObject{{QStringLiteral("uri"), QUrl::fromLocalFile(root).toString()},
                                                            {QStringLiteral("name"), QFileInfo(root).fileName()}}},
            {QStringLiteral("workingDirectory"), QJsonObject{{QStringLiteral("directory"), root}}},
            {QStringLiteral("problems"), QJsonObject{{QStringLiteral("shortenToSingleLine"), false}}},
            {QStringLiteral("codeAction"),
             QJsonObject{{QStringLiteral("disableRuleComment"), QJsonObject{{QStringLiteral("enable"), true}, {QStringLiteral("location"), QStringLiteral("separateLine")}}},
                         {QStringLiteral("showDocumentation"), QJsonObject{{QStringLiteral("enable"), true}}}}},
            {QStringLiteral("file"), file}};
    }
    if (serverId == QLatin1String("python")) {
        // basedpyright asks for the sections "python" and "basedpyright"; both carry the checking options under
        // "analysis". "python" also names the interpreter: the project's venv, so imports of installed packages resolve.
        // Nothing is written into the project.
        const QJsonObject analysis{{QStringLiteral("typeCheckingMode"), QStringLiteral("standard")}, // basedpyright's own default is the very noisy "recommended"
                                   {QStringLiteral("autoImportCompletions"), true},
                                   {QStringLiteral("autoSearchPaths"), true},
                                   {QStringLiteral("useLibraryCodeForTypes"), true},
                                   {QStringLiteral("diagnosticMode"), QStringLiteral("openFilesOnly")},
                                   {QStringLiteral("disableOrganizeImports"), true}}; // Ruff organizes imports
        QJsonObject out{{QStringLiteral("analysis"), analysis}};
        if (section == QLatin1String("python")) {
            const QString py = PythonEnv::venvPython(root, PythonEnv::activeVenv(root));
            if (!py.isEmpty())
                out.insert(QStringLiteral("pythonPath"), py);
        } else if (section != QLatin1String("basedpyright") && section != QLatin1String("python.analysis") &&
                   section != QLatin1String("basedpyright.analysis")) {
            return QJsonObject();
        } else if (section.endsWith(QLatin1String(".analysis"))) {
            return analysis;
        }
        return out;
    }
    if (serverId == QLatin1String("kotlin")) {
        // The Kotlin server builds its inlay hints from a map of option id -> bool in this section (null makes the request
        // fail) and asks again for every request, so toggling a hint class in QODE takes effect without a restart.
        if (section != QLatin1String("jetbrains.kotlin"))
            return QJsonValue::Null;
        const auto &cfg = SettingsManager::instance();
        const bool params = cfg.inlayParameterHints(), types = cfg.inlayTypeHints();
        return QJsonObject{{QStringLiteral("hints.parameters"), params},
                           {QStringLiteral("hints.parameters.compiled"), params}, // arguments of library functions (Compose's Spacer, Button ...)
                           {QStringLiteral("hints.type.property"), types},
                           {QStringLiteral("hints.type.variable"), types},
                           {QStringLiteral("hints.type.function.return"), types},
                           {QStringLiteral("hints.type.function.parameter"), types},
                           {QStringLiteral("hints.lambda.return"), types},
                           {QStringLiteral("hints.lambda.receivers.parameters"), types}};
    }
    if (serverId == QLatin1String("tailwindcss")) {
        if (section == QLatin1String("editor"))
            return QJsonObject{{QStringLiteral("tabSize"), SettingsManager::instance().tabSize()}};
        if (section == QLatin1String("tailwindCSS"))
            return QJsonObject{{QStringLiteral("validate"), true}, {QStringLiteral("emmetCompletions"), false},
                               {QStringLiteral("classAttributes"), QJsonArray{QStringLiteral("class"), QStringLiteral("className"), QStringLiteral("ngClass"), QStringLiteral(":class")}},
                               {QStringLiteral("includeLanguages"), QJsonObject()}};
        return QJsonObject();
    }
    return QJsonValue::Null;
}

// jdtls settings: .project / .classpath / .settings stay out of the user's project (they go to the workspace folder in
// the cache; the JVM property in LspServers::jdtlsCommand is the other half of that), and a changed pom.xml / build.gradle
// is reloaded without a prompt.
QJsonObject javaSettings()
{
    return {{QStringLiteral("java"),
             QJsonObject{{QStringLiteral("import"), QJsonObject{{QStringLiteral("generatesMetadataFilesAtProjectRoot"), false}}},
                         {QStringLiteral("configuration"), QJsonObject{{QStringLiteral("updateBuildConfiguration"), QStringLiteral("automatic")}}},
                         // "N references" / "N implementations" over types and methods (code lenses; off in jdtls by default).
                         // Older servers read implementationsCodeLens.enabled, newer ones implementationCodeLens.
                         {QStringLiteral("referencesCodeLens"), QJsonObject{{QStringLiteral("enabled"), true}}},
                         {QStringLiteral("implementationsCodeLens"), QJsonObject{{QStringLiteral("enabled"), true}}},
                         {QStringLiteral("implementationCodeLens"), QStringLiteral("all")}}}};
}

// `File f = File(".");` -- jdtls says "The method File(String) is undefined for the type Main" and offers only "Create method".
// When the name starts with a capital, is followed by "(" and is not a member call, the likely cause is a forgotten `new`.
bool missingNew(const QTextDocument *td, int line, int column, const QString &message)
{
    static const QRegularExpression re(QStringLiteral("^The method ([A-Z]\\w*)\\(.*\\) is undefined for the type "));
    const auto m = re.match(message);
    const QTextBlock b = td ? td->findBlockByNumber(line) : QTextBlock();
    if (!m.hasMatch() || !b.isValid())
        return false;
    const QString text = b.text(), name = m.captured(1);
    if (!text.mid(column).startsWith(name + QLatin1Char('(')))
        return false;
    const QString before = text.left(column).trimmed();
    return !before.endsWith(QLatin1Char('.')) && !before.endsWith(QLatin1String("new"));
}

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
    connect(m_changeTimer, &QTimer::timeout, this, [this] { flushChanges(); });

    // Watchdog: a 20 ms timer that notices when the UI thread was blocked (typing feels late) and notes it in the
    // server logs next to the request timings.
    auto *watch = new QTimer(this);
    watch->setInterval(20);
    auto *clock = new QElapsedTimer;
    clock->start();
    connect(watch, &QTimer::timeout, this, [this, clock] {
        const qint64 gap = clock->restart();
        if (gap < 80)
            return;
        for (const Server &s : std::as_const(m_servers))
            if (s.client && s.client->isRunning())
                s.client->logTiming(QStringLiteral("UI thread was blocked for %1 ms").arg(gap));
    });
    connect(this, &QObject::destroyed, this, [clock] { delete clock; });
    watch->start();
}

LspManager::~LspManager()
{
    // Stop the servers, but tell no one: the receivers (MainWindow and what it owns) may be half destroyed already.
    blockSignals(true);
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

void LspManager::setJsonSchemas(const QJsonArray &schemas)
{
    m_jsonSchemas = schemas;
    auto it = m_servers.find(QStringLiteral("json"));
    if (it != m_servers.end() && it->client && it->state.status == Status::Running)
        sendJsonSchemas(it.value());
}

void LspManager::sendJsonSchemas(Server &s)
{
    s.client->notify(QStringLiteral("workspace/didChangeConfiguration"),
                     QJsonObject{{QStringLiteral("settings"), QJsonObject{{QStringLiteral("json"), QJsonObject{{QStringLiteral("schemas"), m_jsonSchemas}}}}}});
}

void LspManager::shutdown()
{
    for (auto it = m_servers.begin(); it != m_servers.end(); ++it)
        stopServer(it.value());
    m_servers.clear();
    const QList<QString> paths = m_diagnostics.keys();
    m_diagnostics.clear();
    m_diagBySource.clear();
    for (const QString &p : paths)
        emit diagnosticsChanged(p);
    for (Tracked &t : m_tracked)
        t.openOn.clear();
    emit statusChanged();
}

void LspManager::stopServer(Server &s)
{
    if (!s.client)
        return;
    s.client->disconnect(this);
    s.lastLog = s.client->logLines();
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
    QString nodeDir;
    if (s.spec->needsNode) {
        const QString node = LspServers::nodeExecutable();
        if (node.isEmpty()) {
            setStatus(s, Status::NotFound, tr("Node.js not found — install Node.js (the server is a Node.js program)"));
            return;
        }
        nodeDir = QFileInfo(node).absolutePath();
    }
    QJsonObject options;
    if (s.spec->fallbackFlags) {
        QStringList flags;
        if (!LspServers::hasCompileDatabase(rootPath)) {
            const QmakeProject qmake = QmakeProject::detect(rootPath);
            flags = qmake.compilerFlags();
        }
        for (const QString &dir : includePaths())
            flags << QStringLiteral("-I") + resolveIncludePath(dir, rootPath);
        flags += parseFlags(projectFlagsText(), rootPath); // the user's lines come last and win
        if (!flags.isEmpty())
            options.insert(QStringLiteral("fallbackFlags"), QJsonArray::fromStringList(flags));
    }
    // The vscode html/css/json servers only offer formatting when asked to.
    if (s.spec->id == QLatin1String("html") || s.spec->id == QLatin1String("css") || s.spec->id == QLatin1String("json"))
        options.insert(QStringLiteral("provideFormatter"), true);
    if (s.spec->id == QLatin1String("typescript")) {
        options.insert(QStringLiteral("preferences"), tsPreferences());
        // typescript-language-server does not look next to itself for TypeScript: use the project's own version when it
        // has one (so the editor agrees with its build), otherwise the copy installed together with the server.
        const QString rel = QStringLiteral("node_modules/typescript/lib/tsserver.js");
        for (const QString &dir : {rootPath, LspServers::managedDir(s.spec->managedId)})
            if (QFileInfo::exists(dir + QLatin1Char('/') + rel)) {
                options.insert(QStringLiteral("tsserver"), QJsonObject{{QStringLiteral("path"), dir + QLatin1Char('/') + rel}});
                break;
            }
    }
    // kotlin-lsp only lists its "Run" / "Debug" lenses over main functions when the client says it runs them.
    if (s.spec->id == QLatin1String("kotlin"))
        options.insert(QStringLiteral("runMainCodeLens"), true);
    // {cache}: a folder per server and project for the server's own indexes (survives restarts, never in the project).
    QStringList args = s.spec->arguments;
    if (args.join(QLatin1Char(' ')).contains(QLatin1String("{cache}"))) {
        const QString key = QString::fromLatin1(QCryptographicHash::hash(rootPath.toUtf8(), QCryptographicHash::Md5).toHex().left(12));
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE-AGENTIC/lsp/") +
                              s.spec->id + QLatin1Char('/') + key;
        QDir().mkpath(cache);
        args.replaceInStrings(QStringLiteral("{cache}"), cache);
    }
    if (s.spec->fallbackFlags) {
        // fallbackFlags are ignored for files a compile database describes: give clangd a patched copy instead.
        const QString dir = preparePatchedDatabase(rootPath);
        if (!dir.isEmpty())
            args << QStringLiteral("--compile-commands-dir=") + dir;
    }
    QString program = exe;
    if (s.spec->id == QLatin1String("java")) {
        // jdtls: java -jar <launcher> ...; its configuration and workspace data live in the cache, one per project.
        const QString key = QString::fromLatin1(QCryptographicHash::hash(rootPath.toUtf8(), QCryptographicHash::Md5).toHex().left(12));
        const QString workspace = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE-AGENTIC/lsp/java/") + key;
        QDir().mkpath(workspace);
        QString error;
        if (!LspServers::jdtlsCommand(exe, workspace, &program, &args, &error)) {
            setStatus(s, Status::Crashed, error);
            return;
        }
        // Report progress and serve class files that have no source (java/classFileContents); the settings are also
        // sent as didChangeConfiguration once the server is ready (see onServerReady), which is when jdtls reads them.
        options.insert(QStringLiteral("settings"), javaSettings());
        options.insert(QStringLiteral("extendedClientCapabilities"),
                       QJsonObject{{QStringLiteral("progressReportProvider"), true}, {QStringLiteral("classFileContentsSupport"), true}});
    }
    s.progress.clear();
    updateProgressState(s);
    s.client = new LspClient(program, args, rootPath, options, this);
    if (s.spec->wantsConfiguration) {
        const QString id = s.spec->id;
        s.client->setConfigurationProvider([id, rootPath](const QJsonObject &item) { return configurationFor(id, item, rootPath); });
    }
    if (!nodeDir.isEmpty())
        s.client->prependToPath(nodeDir);
    s.client->setApplyEditHandler([this](const QJsonObject &edit) { return handleApplyEdit(edit); });
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
    if (s.spec->id == QLatin1String("java"))
        s.client->notify(QStringLiteral("workspace/didChangeConfiguration"), QJsonObject{{QStringLiteral("settings"), javaSettings()}});
    if (s.spec->id == QLatin1String("json") && !m_jsonSchemas.isEmpty())
        sendJsonSchemas(s);
    for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it)
        if (it->servers().contains(id) && !it->openOn.contains(id))
            sendOpen(it.key(), it.value(), id);
}

void LspManager::onServerStopped(const QString &id, bool crashed)
{
    Server &s = m_servers[id];
    const QString error = s.client ? s.client->errorString() : QString();
    if (s.client) {
        s.lastLog = s.client->logLines();
        s.client->deleteLater();
        s.client = nullptr;
    }
    s.progress.clear();
    updateProgressState(s);
    for (Tracked &t : m_tracked) {
        t.openOn.remove(id);
        if (t.serverId == id)
            t.reported = -1; // a new process reports afresh
    }
    for (const Tracked &t : std::as_const(m_tracked))
        if (t.servers().contains(id))
            clearDiagnosticsFor(t.path, id);
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
                    if (t.servers().contains(id)) {
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
        t.openOn.remove(serverId);
    QString root = m_root;
    if (root.isEmpty())
        for (const Tracked &t : std::as_const(m_tracked))
            if (t.servers().contains(serverId)) {
                root = QFileInfo(t.path).absolutePath();
                break;
            }
    // Only start when a document needs it; otherwise wait for the next one.
    bool needed = false;
    for (const Tracked &t : std::as_const(m_tracked))
        needed = needed || t.servers().contains(serverId);
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

QList<LspManager::ServerState> LspManager::servers(bool resolve) const
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
            // locate() walks PATH, nvm and JDK folders; this runs on every status change, so remember the answer a while.
            auto &cached = m_locateCache[spec.id];
            if (resolve && (!cached.second.isValid() || cached.second.elapsed() > 10000)) {
                cached.first = LspServers::locate(spec, serverPath(spec.id));
                cached.second.start();
            }
            st.path = cached.first;
            if (st.path.isEmpty() && cached.second.isValid())
                st.status = Status::NotFound;
        }
        st.installable = spec.installable;
        st.companion = spec.companion;
        for (const Tracked &t : m_tracked)
            st.documents += t.servers().contains(spec.id);
        out << st;
    }
    return out;
}

QStringList LspManager::logOf(const QString &serverId) const
{
    const auto it = m_servers.constFind(serverId);
    if (it == m_servers.constEnd())
        return {};
    return it->client ? it->client->logLines() : it->lastLog;
}

QString LspManager::serverIdFor(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    return it == m_tracked.constEnd() ? QString() : it->serverId;
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

QStringList LspManager::includePaths() const
{
    QStringList out;
    for (const QString &e : SettingsManager::instance().includePaths())
        out << resolveIncludePath(e, m_root);
    return out;
}

void LspManager::setIncludePaths(const QStringList &paths)
{
    SettingsManager::instance().setIncludePaths(paths);
    for (const LspServerSpec &spec : LspServers::all())
        if (spec.fallbackFlags && m_servers.contains(spec.id))
            restart(spec.id);
}

QString LspManager::resolveIncludePath(const QString &entry, const QString &projectRoot)
{
    QString p = entry.trimmed();
    p.replace(QStringLiteral("{project}"), projectRoot);
    if (p.startsWith(QStringLiteral("~/")))
        p = QDir::homePath() + p.mid(1);
    if (QDir::isRelativePath(p) && !projectRoot.isEmpty())
        p = projectRoot + QLatin1Char('/') + p;
    return QDir::cleanPath(p);
}

QString LspManager::compileDatabasePath(const QString &root)
{
    if (root.isEmpty())
        return {};
    for (const QString &rel : {QStringLiteral("compile_commands.json"), QStringLiteral("build/compile_commands.json")}) {
        const QString path = root + QLatin1Char('/') + rel;
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

// A shell-quoted word for the "command" string of a compile_commands.json entry.
static QString shellQuote(const QString &word)
{
    QString q = word;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString LspManager::preparePatchedDatabase(const QString &root)
{
    m_patchSource.clear();
    m_patchTarget.clear();
    if (m_dbWatcher && !m_dbWatcher->files().isEmpty())
        m_dbWatcher->removePaths(m_dbWatcher->files());
    const QString source = compileDatabasePath(root);
    if (source.isEmpty() || includePaths().isEmpty())
        return {};
    const QString key = QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Md5).toHex().left(12));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                        QStringLiteral("/QODE-AGENTIC/lsp/clangd-db/") + key;
    QDir().mkpath(dir);
    m_patchSource = source;
    m_patchTarget = dir + QStringLiteral("/compile_commands.json");
    if (!m_dbWatcher) {
        m_dbWatcher = new QFileSystemWatcher(this);
        m_dbTimer = new QTimer(this);
        m_dbTimer->setSingleShot(true);
        m_dbTimer->setInterval(500);
        connect(m_dbTimer, &QTimer::timeout, this, &LspManager::rewritePatchedDatabase);
        connect(m_dbWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_dbTimer->start(); });
    }
    rewritePatchedDatabase();
    if (!QFileInfo::exists(m_patchTarget))
        return {};
    return dir;
}

void LspManager::rewritePatchedDatabase()
{
    if (m_patchSource.isEmpty())
        return;
    // CMake replaces the file rather than editing it, which drops the watch.
    if (m_dbWatcher && QFileInfo::exists(m_patchSource) && !m_dbWatcher->files().contains(m_patchSource))
        m_dbWatcher->addPath(m_patchSource);
    QFile in(m_patchSource);
    if (!in.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    in.close();
    if (!doc.isArray())
        return;
    QStringList extra;
    for (const QString &d : includePaths())
        extra << QStringLiteral("-I") + d;
    QString quoted;
    for (const QString &f : extra)
        quoted += QLatin1Char(' ') + shellQuote(f);

    QJsonArray out;
    for (const QJsonValue &v : doc.array()) {
        QJsonObject e = v.toObject();
        if (e.contains(QStringLiteral("arguments"))) {
            QStringList args;
            for (const QJsonValue &a : e.value(QStringLiteral("arguments")).toArray())
                args << a.toString();
            int at = args.indexOf(QStringLiteral("--")); // anything after "--" is an input file
            if (at < 0)
                at = args.size();
            for (int i = 0; i < extra.size(); ++i)
                args.insert(at + i, extra[i]);
            e.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(args));
        } else if (e.contains(QStringLiteral("command"))) {
            e.insert(QStringLiteral("command"), e.value(QStringLiteral("command")).toString() + quoted);
        }
        out.append(e);
    }
    QSaveFile f(m_patchTarget);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
        f.commit();
    }
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
    if (!spec || doc->isLarge())
        return; // a large file would be parsed again on every edit, and its tokens / symbols cross the pipe each time
    if (JarSource::isLibraryPath(doc->filePath()) && JarSource::uriForPath(doc->filePath()).isEmpty())
        return; // unpacked in an earlier session: the server cannot map it back to its jar
    Tracked t;
    t.path = doc->filePath();
    t.uri = uriFor(t.path);
    t.serverId = spec->id;
    const QList<const LspServerSpec *> companions = LspServers::companionsFor(t.path, rootFor(t.path));
    for (const LspServerSpec *c : companions)
        t.companions << c->id;
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
        for (const QString &id : t.servers()) {
            Server &s = m_servers[id];
            if (t.openOn.contains(id) && s.client && s.client->isRunning())
                s.client->notify(QStringLiteral("textDocument/didClose"),
                                 QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), t.uri}}}});
        }
        clearDiagnosticsFor(t.path);
    });

    for (const LspServerSpec *c : companions)
        ensureServer(*c, t.path);
    Server *s = ensureServer(*spec, t.path);
    for (const QString &id : m_tracked[doc].servers()) {
        const Server &srv = m_servers[id];
        if (srv.client && srv.client->isRunning() && srv.state.status == Status::Running)
            sendOpen(doc, m_tracked[doc], id);
    }
    if (!(s && s->client && s->client->isRunning()))
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
    for (const QString &id : t.servers()) {
        Server &s = m_servers[id];
        if (sendClose && t.openOn.contains(id) && s.client && s.client->isRunning())
            s.client->notify(QStringLiteral("textDocument/didClose"),
                             QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), t.uri}}}});
    }
    clearDiagnosticsFor(t.path);
}

void LspManager::sendOpen(Document *doc, Tracked &t, const QString &serverId)
{
    Server &s = m_servers[serverId];
    if (!s.client || !s.client->isRunning())
        return;
    // The servers already holding the document get the pending edits first: all of them then have the same text at the
    // same version, which `sent` describes.
    if (t.dirty && !t.openOn.isEmpty() && !t.openOn.contains(serverId))
        flushDocument(doc, t, false);
    t.version = qMax(t.version, 1);
    t.openOn.insert(serverId);
    t.dirty = false;
    t.sent = doc->text();
    if (serverId == t.serverId) {
        t.tokensResultId.clear(); // a new server process knows no earlier result
        t.tokensData.clear();
    }
    s.client->notify(QStringLiteral("textDocument/didOpen"),
                     QJsonObject{{QStringLiteral("textDocument"),
                                  QJsonObject{{QStringLiteral("uri"), t.uri},
                                              {QStringLiteral("languageId"), s.spec->languageId(t.path)},
                                              {QStringLiteral("version"), t.version},
                                              {QStringLiteral("text"), t.sent}}}});
    pullDiagnostics(doc);
}

void LspManager::flushChanges(bool pull)
{
    for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it)
        flushDocument(it.key(), it.value(), pull);
}

// Servers that sync incrementally get only the edited range (what differs from the text they have), the others the whole
// text. A server then works on a keystroke's worth of change instead of reading the whole file again.
void LspManager::flushDocument(Document *doc, Tracked &t, bool pull)
{
    if (!t.openOn.isEmpty() && t.pullDue && pull) {
        t.pullDue = false;
        pullDiagnostics(doc);
    }
    if (!t.dirty || t.openOn.isEmpty())
        return;
    t.dirty = false;
    const QString text = doc->text();
    QJsonObject ranged;
    bool rangedDone = false;
    for (const QString &id : t.servers()) {
        Server &s = m_servers[id];
        if (!t.openOn.contains(id) || !s.client || !s.client->isRunning())
            continue;
        if (!rangedDone && LspSync::wantsIncremental(s.client->serverCapabilities())) {
            ranged = LspSync::contentChange(t.sent, text);
            rangedDone = true;
            if (ranged.isEmpty())
                return; // typed and undone again: the servers already have this text
        }
    }
    ++t.version;
    for (const QString &id : t.servers()) {
        Server &s = m_servers[id];
        if (!t.openOn.contains(id) || !s.client || !s.client->isRunning())
            continue;
        const QJsonObject change = LspSync::wantsIncremental(s.client->serverCapabilities()) ? ranged
                                                                                             : QJsonObject{{QStringLiteral("text"), text}};
        s.client->notify(QStringLiteral("textDocument/didChange"),
                         QJsonObject{{QStringLiteral("textDocument"),
                                      QJsonObject{{QStringLiteral("uri"), t.uri}, {QStringLiteral("version"), t.version}}},
                                     {QStringLiteral("contentChanges"), QJsonArray{change}}});
    }
    t.sent = text;
    if (pull)
        pullDiagnostics(doc);
    else
        t.pullDue = true; // sent early for a completion: the diagnostics follow with the debounced flush
}

void LspManager::documentSaved(Document *doc)
{
    auto it = m_tracked.find(doc);
    if (it == m_tracked.end() || it->openOn.isEmpty())
        return;
    flushChanges();
    for (const QString &id : it->servers()) {
        Server &s = m_servers[id];
        if (it->openOn.contains(id) && s.client && s.client->isRunning())
            s.client->notify(QStringLiteral("textDocument/didSave"),
                             QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), it->uri}}}});
    }
    pullDiagnostics(doc);
}

bool LspManager::isJvmBuildFile(const QString &path)
{
    const QString n = QFileInfo(path).fileName();
    return n == QLatin1String("build.gradle") || n == QLatin1String("build.gradle.kts") || n == QLatin1String("settings.gradle")
           || n == QLatin1String("settings.gradle.kts") || n == QLatin1String("gradle.properties") || n == QLatin1String("pom.xml")
           || n == QLatin1String("gradle-wrapper.properties") || n.endsWith(QLatin1String(".versions.toml"));
}

// The build files of the project: the root's and its direct sub-folders' (modules), plus the version catalog.
QStringList LspManager::jvmBuildFiles() const
{
    QStringList out;
    if (m_root.isEmpty())
        return out;
    const QStringList names = {QStringLiteral("build.gradle"), QStringLiteral("build.gradle.kts"), QStringLiteral("settings.gradle"),
                               QStringLiteral("settings.gradle.kts"), QStringLiteral("gradle.properties"), QStringLiteral("pom.xml")};
    QStringList dirs = {m_root};
    for (const QString &d : QDir(m_root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (!d.startsWith(QLatin1Char('.')) && d != QLatin1String("build") && d != QLatin1String("node_modules") && d != QLatin1String("target"))
            dirs << m_root + QLatin1Char('/') + d;
    for (const QString &dir : std::as_const(dirs))
        for (const QString &n : names)
            if (QFileInfo::exists(dir + QLatin1Char('/') + n))
                out << dir + QLatin1Char('/') + n;
    const QString catalog = m_root + QStringLiteral("/gradle/libs.versions.toml");
    if (QFileInfo::exists(catalog))
        out << catalog;
    return out;
}

bool LspManager::hasBuildFiles() const
{
    return !jvmBuildFiles().isEmpty();
}

bool LspManager::canSyncBuildFiles() const
{
    for (const char *id : {"java", "kotlin"}) {
        const auto it = m_servers.constFind(QLatin1String(id));
        if (it != m_servers.constEnd() && it->client && it->client->isRunning() && it->state.status == Status::Running)
            return true;
    }
    return false;
}

bool LspManager::syncBuildFiles(bool manual)
{
    const QStringList files = jvmBuildFiles();
    if (files.isEmpty())
        return false;
    bool any = false;
    for (const char *name : {"java", "kotlin"}) {
        const QString id = QLatin1String(name);
        auto it = m_servers.find(id);
        if (it == m_servers.end() || !it->client || !it->client->isRunning() || it->state.status != Status::Running)
            continue; // a server that is not up reads the fresh files when it starts
        Server &s = *it;
        any = true;
        s.state.syncing = true;
        s.syncSawProgress = false;
        const int gen = ++s.syncGen;
        QJsonArray changes;
        for (const QString &f : files)
            changes.append(QJsonObject{{QStringLiteral("uri"), QUrl::fromLocalFile(f).toString()}, {QStringLiteral("type"), 2}});
        s.client->notify(QStringLiteral("workspace/didChangeWatchedFiles"), QJsonObject{{QStringLiteral("changes"), changes}});
        if (manual && id == QLatin1String("java"))
            s.client->request(QStringLiteral("workspace/executeCommand"), QJsonObject{{QStringLiteral("command"), QStringLiteral("java.project.import")}});
        emit statusChanged();
        // Nothing reported within a few seconds: the server did not notice. A manual sync then restarts it (it re-reads
        // everything on start); an automatic one just stops claiming to sync.
        QTimer::singleShot(5000, this, [this, id, gen, manual] {
            Server &srv = m_servers[id];
            if (srv.syncGen != gen || !srv.state.syncing || srv.syncSawProgress)
                return;
            srv.state.syncing = false;
            emit statusChanged();
            if (manual) {
                restart(id);
                srv.state.syncing = true; // until the restarted server is idle again
                srv.syncSawProgress = true;
                emit statusChanged();
                const int g = ++srv.syncGen; // a restarted server's own import ends the sync; otherwise give up after a while
                QTimer::singleShot(90000, this, [this, id, g] {
                    Server &r = m_servers[id];
                    if (r.syncGen == g && r.state.syncing) {
                        r.state.syncing = false;
                        emit statusChanged();
                    }
                });
            }
        });
    }
    return any;
}

// Progress arrives in floods while a project is indexed; the status bar and everything hanging off it update at most
// every 250 ms.
void LspManager::scheduleStatusChanged()
{
    if (m_statusPending)
        return;
    m_statusPending = true;
    QTimer::singleShot(250, this, [this] {
        m_statusPending = false;
        emit statusChanged();
    });
}

// The operations a server runs, as the status bar and the LSP menu show them: the newest first.
void LspManager::updateProgressState(Server &s)
{
    QList<const Server::Progress *> ops;
    for (const Server::Progress &p : std::as_const(s.progress))
        ops.append(&p);
    std::sort(ops.begin(), ops.end(), [](const Server::Progress *a, const Server::Progress *b) { return a->order > b->order; });
    s.state.operations.clear();
    for (const Server::Progress *p : ops) {
        QString text = p->title;
        if (!p->text.isEmpty())
            text += (text.isEmpty() ? QString() : QStringLiteral(" — ")) + p->text;
        if (p->percent >= 0 && !text.contains(QLatin1Char('%'))) // jdtls' messages already say "0%"
            text += QStringLiteral(" %1%").arg(p->percent);
        s.state.operations.append(text.trimmed());
    }
    s.state.progress = s.state.operations.value(0);
    s.state.percent = ops.isEmpty() ? -1 : ops.first()->percent;
}

// Progress of a server changed: a sync that was running is finished once every operation has ended for a moment.
void LspManager::progressChanged(Server &s)
{
    if (!s.progress.isEmpty()) {
        if (s.state.syncing)
            s.syncSawProgress = true;
        return;
    }
    if (!s.state.syncing || !s.syncSawProgress)
        return;
    const QString id = s.state.id;
    const int gen = ++s.syncGen;
    QTimer::singleShot(1500, this, [this, id, gen] {
        Server &srv = m_servers[id];
        if (srv.syncGen != gen || !srv.progress.isEmpty() || !srv.state.syncing)
            return;
        srv.state.syncing = false;
        emit statusChanged();
        emit buildSyncFinished(srv.state.name.section(QLatin1Char(' '), 0, 0));
    });
}

// A qmake .pro / .pri changed: the flags derived from it are stale, so servers using them restart. A Gradle / Maven
// file makes the Java and Kotlin servers re-import the project.
void LspManager::buildFileSaved(const QString &path)
{
    if (isJvmBuildFile(path)) {
        syncBuildFiles(false);
        return;
    }
    const QString suffix = QFileInfo(path).suffix();
    if (suffix != QLatin1String("pro") && suffix != QLatin1String("pri"))
        return;
    for (const LspServerSpec &spec : LspServers::all())
        if (spec.fallbackFlags && m_servers.contains(spec.id) && m_servers[spec.id].client)
            restart(spec.id);
}

// Save As / rename: the server knows the file under its old URI, so close it and open it again.
void LspManager::filesMoved(const QList<QPair<QString, QString>> &moves, const QStringList &rewritten)
{
    // The watched-files events, the same for every server: a moved folder counts as its files (up to a limit).
    constexpr int kMaxFiles = 5000;
    QJsonArray changes;
    auto add = [&changes](const QString &path, int type) { // 1 created, 2 changed, 3 deleted
        changes.append(QJsonObject{{QStringLiteral("uri"), QUrl::fromLocalFile(path).toString()}, {QStringLiteral("type"), type}});
    };
    for (const auto &m : moves) {
        if (!QFileInfo(m.second).isDir()) {
            add(m.first, 3);
            add(m.second, 1);
            continue;
        }
        QDirIterator it(m.second, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && changes.size() < 2 * kMaxFiles) {
            const QString now = it.next();
            add(m.first + now.mid(m.second.size()), 3);
            add(now, 1);
        }
    }
    for (const QString &f : rewritten)
        add(f, 2);

    for (Server &s : m_servers) {
        if (!s.client || !s.client->isRunning() || s.state.status != Status::Running)
            continue;
        const QJsonObject didRename = s.client->serverCapabilities()
                                          .value(QStringLiteral("workspace")).toObject()
                                          .value(QStringLiteral("fileOperations")).toObject()
                                          .value(QStringLiteral("didRename")).toObject();
        QJsonArray files;
        if (!didRename.isEmpty())
            for (const auto &m : moves)
                if (LspSync::fileOperationMatches(didRename, m.second, QFileInfo(m.second).isDir()))
                    files.append(QJsonObject{{QStringLiteral("oldUri"), QUrl::fromLocalFile(m.first).toString()},
                                             {QStringLiteral("newUri"), QUrl::fromLocalFile(m.second).toString()}});
        if (!files.isEmpty())
            s.client->notify(QStringLiteral("workspace/didRenameFiles"), QJsonObject{{QStringLiteral("files"), files}});
        if (!changes.isEmpty())
            s.client->notify(QStringLiteral("workspace/didChangeWatchedFiles"), QJsonObject{{QStringLiteral("changes"), changes}});
    }
}

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
    if (it == m_tracked.constEnd() || !it->opened())
        return nullptr;
    const Server &s = m_servers[it->serverId];
    if (!s.client || !s.client->isRunning() || s.state.status != Status::Running)
        return nullptr;
    flushChanges(); // the server must see the text the cursor position refers to
    *uri = it->uri;
    return s.client;
}

QList<LspClient *> LspManager::readyClientsFor(Document *doc, QString *uri)
{
    QList<LspClient *> out;
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd())
        return out;
    flushChanges();
    *uri = it->uri;
    for (const QString &id : it->servers()) {
        const Server &s = m_servers[id];
        if (it->openOn.contains(id) && s.client && s.client->isRunning() && s.state.status == Status::Running)
            out << s.client;
    }
    return out;
}

bool LspManager::isServed(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    return it != m_tracked.constEnd() && it->opened();
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
    loc.fromClass = JarSource::isJarUri(uri) && JarSource::lastWasClass();
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
    it.documentation = markupText(o.value(QStringLiteral("documentation"))).trimmed();
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
    flushChanges(false); // the server must complete the text the caret refers to, not the text from 200 ms ago
    QString uri;
    QList<LspClient *> clients;
    for (LspClient *c : readyClientsFor(doc, &uri))
        if (c->serverCapabilities().contains(QStringLiteral("completionProvider")))
            clients << c;
    if (clients.isEmpty()) {
        done({}, false);
        return;
    }
    for (const auto &r : std::as_const(m_completionRequests))
        if (r.first)
            r.first->cancel(r.second);
    m_completionRequests.clear();
    QJsonObject context{{QStringLiteral("triggerKind"), triggerKind}};
    if (triggerKind == 2 && !triggerChar.isEmpty())
        context.insert(QStringLiteral("triggerCharacter"), triggerChar);
    QJsonObject params = positionParams(uri, line, column);
    params.insert(QStringLiteral("context"), context);
    // The main server and companions (Tailwind class names ...) answer separately: show their union. A slow companion
    // must not hold the popup back, so whatever arrived after 400 ms is delivered and the rest dropped.
    struct Gather {
        int pending = 0;
        bool delivered = false, incomplete = false;
        QVector<LspCompletionItem> items;
        std::function<void(const QVector<LspCompletionItem> &, bool)> done;
        void deliver()
        {
            if (delivered)
                return;
            delivered = true;
            done(items, incomplete);
        }
    };
    auto gather = std::make_shared<Gather>();
    gather->pending = clients.size();
    gather->done = std::move(done);
    for (LspClient *c : clients) {
        // Servers that resolve items (completionItem/resolve): the details panel asks for documentation, and jdtls /
        // typescript-language-server add an item's import (additionalTextEdits) only there.
        bool resolvable = false, onAccept = false;
        QString serverId;
        for (auto it = m_servers.constBegin(); it != m_servers.constEnd(); ++it)
            if (it->client == c && it->spec) {
                serverId = it.key();
                resolvable = c->serverCapabilities().value(QStringLiteral("completionProvider")).toObject().value(QStringLiteral("resolveProvider")).toBool();
                onAccept = resolvable && it->spec->resolvesCompletions;
            }
        const int id = c->request(QStringLiteral("textDocument/completion"), params,
                                  [gather, resolvable, onAccept, serverId](const QJsonValue &result, const QJsonObject &error) {
                                      if (error.isEmpty()) {
                                          QJsonArray items;
                                          if (result.isArray()) {
                                              items = result.toArray();
                                          } else if (result.isObject()) {
                                              gather->incomplete = gather->incomplete || result.toObject().value(QStringLiteral("isIncomplete")).toBool();
                                              items = result.toObject().value(QStringLiteral("items")).toArray();
                                          }
                                          gather->items.reserve(gather->items.size() + items.size());
                                          for (const QJsonValue &v : items) {
                                              gather->items << parseCompletionItem(v.toObject());
                                              LspCompletionItem &last = gather->items.last();
                                              last.server = serverId;
                                              if (resolvable) {
                                                  last.resolveData = v.toObject();
                                                  last.resolveOnAccept = onAccept;
                                              }
                                          }
                                      }
                                      if (--gather->pending == 0)
                                          gather->deliver();
                                  });
        m_completionRequests.append({QPointer<LspClient>(c), id});
    }
    if (clients.size() > 1)
        QTimer::singleShot(400, this, [gather] { gather->deliver(); });
}

QHash<QString, QVector<LspTextEdit>> LspManager::editsOf(const QJsonObject &edit, bool *ok, bool allowFileOps)
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
        if (o.contains(QStringLiteral("kind"))) { // create / rename / delete file
            if (!allowFileOps)
                *ok = false;
            continue;
        }
        *ok = add(o.value(QStringLiteral("textDocument")).toObject().value(QStringLiteral("uri")).toString(), o.value(QStringLiteral("edits")).toArray()) && *ok;
    }
    return out;
}

QVector<LspManager::FileOp> LspManager::fileOpsOf(const QJsonObject &edit)
{
    QVector<FileOp> out;
    for (const QJsonValue &v : edit.value(QStringLiteral("documentChanges")).toArray()) {
        const QJsonObject o = v.toObject();
        if (!o.contains(QStringLiteral("kind")))
            continue;
        FileOp op;
        op.kind = o.value(QStringLiteral("kind")).toString();
        const QJsonObject opts = o.value(QStringLiteral("options")).toObject();
        op.overwrite = opts.value(QStringLiteral("overwrite")).toBool();
        if (op.kind == QLatin1String("rename")) {
            op.path = QUrl(o.value(QStringLiteral("oldUri")).toString()).toLocalFile();
            op.target = QUrl(o.value(QStringLiteral("newUri")).toString()).toLocalFile();
        } else {
            op.path = QUrl(o.value(QStringLiteral("uri")).toString()).toLocalFile();
        }
        if (!op.path.isEmpty())
            out << op;
    }
    return out;
}

bool LspManager::handleApplyEdit(const QJsonObject &edit)
{
    if (m_capturing) { // an organize-imports probe: remember the edit, change nothing
        m_captured = edit;
        return false;
    }
    return m_applyEdit && m_applyEdit(edit);
}

// Imports the server's "organize imports" would delete. The Kotlin compiler reports no unused-import diagnostics, so
// run the command with its edit intercepted and compare the import lines before and after.
void LspManager::unusedImports(Document *doc, std::function<void(const QVector<int> &)> done)
{
    static const QString command = QStringLiteral("kotlin.organize.imports");
    QString uri;
    if (m_capturing || !supportsCommand(doc, command)) {
        done({});
        return;
    }
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    const QStringList lines = doc->text().split(QLatin1Char('\n'));
    const QString path = doc->filePath();
    m_capturing = true;
    m_captured = {};
    QPointer<LspClient> guard(c);
    c->request(QStringLiteral("workspace/executeCommand"),
               QJsonObject{{QStringLiteral("command"), command}, {QStringLiteral("arguments"), QJsonArray{uri}}},
               [this, guard, lines, path, done](const QJsonValue &, const QJsonObject &) {
                   m_capturing = false;
                   const QJsonObject edit = std::exchange(m_captured, QJsonObject());
                   QVector<int> unused;
                   bool ok = false;
                   const QVector<LspTextEdit> edits = guard ? editsOf(edit, &ok).value(path) : QVector<LspTextEdit>();
                   QSet<QString> kept;
                   int first = -1, last = -1;
                   for (const LspTextEdit &e : edits) {
                       first = first < 0 ? e.startLine : qMin(first, e.startLine);
                       last = qMax(last, e.endLine);
                       for (const QString &l : e.text.split(QLatin1Char('\n')))
                           kept.insert(l.trimmed());
                   }
                   for (int i = qMax(first, 0); first >= 0 && i < last && i < lines.size(); ++i) {
                       const QString t = lines.at(i).trimmed();
                       if (!t.startsWith(QLatin1String("import ")) || kept.contains(t))
                           continue;
                       // Five or more imports from one package come back folded into `import pkg.*`: still in use.
                       const int dot = t.lastIndexOf(QLatin1Char('.'));
                       if (dot > 0 && !t.contains(QLatin1String(" as ")) && kept.contains(t.left(dot) + QStringLiteral(".*")))
                           continue;
                       unused.append(i);
                   }
                   done(unused);
               });
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

// completionItem/resolve fills in what the server left out of the list (jdtls and typescript-language-server: the import line
// of a symbol that is not imported yet; most servers: the documentation). `done` always runs once: with the resolved item,
// or the original after a timeout / on error.
void LspManager::resolveCompletion(Document *doc, const LspCompletionItem &item, std::function<void(const LspCompletionItem &)> done)
{
    QString uri;
    LspClient *c = nullptr;
    if (!item.resolveData.isEmpty()) {
        const QList<LspClient *> clients = readyClientsFor(doc, &uri);
        for (LspClient *candidate : clients) {
            for (auto it = m_servers.constBegin(); it != m_servers.constEnd(); ++it)
                if (it->client == candidate && (item.server.isEmpty() || it.key() == item.server))
                    c = candidate;
            if (c)
                break;
        }
    }
    if (!c) {
        done(item);
        return;
    }
    auto finished = std::make_shared<bool>(false);
    auto finish = [finished, done](const LspCompletionItem &it) {
        if (*finished)
            return;
        *finished = true;
        done(it);
    };
    QTimer::singleShot(1500, this, [finish, item] { finish(item); });
    c->request(QStringLiteral("completionItem/resolve"), item.resolveData, [finish, item](const QJsonValue &result, const QJsonObject &error) {
        LspCompletionItem out = item;
        if (error.isEmpty() && result.isObject()) {
            const LspCompletionItem fresh = parseCompletionItem(result.toObject());
            out.additionalEdits = fresh.additionalEdits;
            if (!fresh.hasEdit && !fresh.insertText.isEmpty() && !out.hasEdit)
                out.insertText = fresh.insertText;
            if (!fresh.documentation.isEmpty())
                out.documentation = fresh.documentation;
            out.longDetail = fresh.detail;
            out.resolved = true;
        }
        finish(out);
    });
}

void LspManager::codeActions(Document *doc, int startLine, int startColumn, int endLine, int endColumn,
                             std::function<void(const QVector<LspCodeAction> &)> done, const QStringList &only, bool automatic, const QString &serverOnly)
{
    QString uri;
    QList<LspClient *> clients = readyClientsFor(doc, &uri);
    if (!serverOnly.isEmpty()) {
        const Server &only = m_servers[serverOnly];
        clients.removeIf([&only](LspClient *c) { return c != only.client; });
    }
    if (clients.isEmpty()) {
        done({});
        return;
    }
    const QJsonObject range{{QStringLiteral("start"), QJsonObject{{QStringLiteral("line"), startLine}, {QStringLiteral("character"), startColumn}}},
                            {QStringLiteral("end"), QJsonObject{{QStringLiteral("line"), endLine}, {QStringLiteral("character"), endColumn}}}};
    // Every server answers separately (ESLint offers "Fix this rule", the language server offers imports): merge them.
    struct Gather {
        int pending = 0;
        QVector<LspCodeAction> actions;
    };
    auto gather = std::make_shared<Gather>();
    gather->pending = clients.size();
    for (const LspDiagnostic &d : m_diagnostics.value(doc->filePath())) {
        const bool before = d.endLine < startLine || (d.endLine == startLine && d.endColumn < startColumn);
        const bool after = d.startLine > endLine || (d.startLine == endLine && d.startColumn > endColumn);
        if (before || after || d.server != QLatin1String("java") || !missingNew(doc->textDocument(), d.startLine, d.startColumn, d.message))
            continue;
        const QJsonObject at{{QStringLiteral("line"), d.startLine}, {QStringLiteral("character"), d.startColumn}};
        const QJsonObject edit{{QStringLiteral("changes"),
                                QJsonObject{{uri, QJsonArray{QJsonObject{{QStringLiteral("range"), QJsonObject{{QStringLiteral("start"), at}, {QStringLiteral("end"), at}}},
                                                                         {QStringLiteral("newText"), QStringLiteral("new ")}}}}}}};
        const QString title = tr("Add 'new' (create an instance)");
        gather->actions.append({title, QStringLiteral("quickfix"), QJsonObject{{QStringLiteral("title"), title}, {QStringLiteral("edit"), edit}}, QStringLiteral("java")});
    }
    for (LspClient *c : clients) {
        QString serverId;
        for (auto it = m_servers.constBegin(); it != m_servers.constEnd(); ++it)
            if (it->client == c)
                serverId = it.key();
        QJsonArray diagnostics;
        for (const LspDiagnostic &d : m_diagnostics.value(doc->filePath())) {
            const bool before = d.endLine < startLine || (d.endLine == startLine && d.endColumn < startColumn);
            const bool after = d.startLine > endLine || (d.startLine == endLine && d.startColumn > endColumn);
            if (!before && !after && !d.raw.isEmpty() && d.server == serverId)
                diagnostics.append(d.raw);
        }
        QJsonObject context{{QStringLiteral("diagnostics"), diagnostics}, {QStringLiteral("triggerKind"), automatic ? 2 : 1}};
        if (!only.isEmpty())
            context.insert(QStringLiteral("only"), QJsonArray::fromStringList(only));
        c->request(QStringLiteral("textDocument/codeAction"),
                   QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}},
                               {QStringLiteral("range"), range},
                               {QStringLiteral("context"), context}},
                   [gather, done, serverId](const QJsonValue &result, const QJsonObject &error) {
                       if (error.isEmpty())
                           for (const QJsonValue &v : result.toArray()) {
                               const QJsonObject o = v.toObject();
                               // A bare Command has `command` as a string; a CodeAction has a title and optional edit/command.
                               if (o.value(QStringLiteral("title")).toString().isEmpty() || o.value(QStringLiteral("disabled")).isObject())
                                   continue;
                               gather->actions.append({o.value(QStringLiteral("title")).toString(), o.value(QStringLiteral("kind")).toString(), o, serverId});
                           }
                       if (--gather->pending == 0)
                           done(gather->actions);
                   });
    }
}

bool LspManager::formatting(Document *doc, int tabSize, bool spaces, int timeoutMs, QVector<LspTextEdit> *edits)
{
    // The first ready server that can format: the file's main server, else a companion (Ruff formats Python files).
    QString uri;
    LspClient *c = nullptr;
    for (LspClient *candidate : readyClientsFor(doc, &uri)) {
        const QJsonValue provider = candidate->serverCapabilities().value(QStringLiteral("documentFormattingProvider"));
        if (!provider.isUndefined() && !provider.isNull() && provider.toBool(true)) {
            c = candidate;
            break;
        }
    }
    if (!c)
        return false;
    // Shared with the callback, which may still run after a timeout.
    struct State {
        bool done = false, ok = false;
        QVector<LspTextEdit> edits;
        QEventLoop loop;
    };
    auto st = std::make_shared<State>();
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &st->loop, &QEventLoop::quit);
    c->request(QStringLiteral("textDocument/formatting"),
               QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}},
                           {QStringLiteral("options"), QJsonObject{{QStringLiteral("tabSize"), tabSize},
                                                                   {QStringLiteral("insertSpaces"), spaces}}}},
               [st](const QJsonValue &result, const QJsonObject &error) {
                   st->done = true;
                   if (error.isEmpty() && result.isArray()) {
                       st->ok = true;
                       for (const QJsonValue &v : result.toArray())
                           st->edits.append(parseEdit(v.toObject()));
                   }
                   st->loop.quit();
               });
    timeout.start(timeoutMs);
    st->loop.exec();
    if (!st->done || !st->ok)
        return false;
    *edits = st->edits;
    return true;
}

void LspManager::runCodeAction(Document *doc, const LspCodeAction &action, std::function<void(bool)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri); // also flushes pending edits
    if (!action.server.isEmpty() && m_servers.contains(action.server) && m_servers[action.server].client &&
        m_servers[action.server].client->isRunning())
        c = m_servers[action.server].client; // the command belongs to the server that offered the action
    if (!c) {
        done(false);
        return;
    }
    // A lazy action (TypeScript refactors, quick fixes): the list only carries the title and `data`; the edit comes from
    // codeAction/resolve. Servers that do not resolve send the edit up front.
    const QJsonObject json = action.json;
    const QJsonValue provider = c->serverCapabilities().value(QStringLiteral("codeActionProvider"));
    const bool canResolve = provider.isObject() && provider.toObject().value(QStringLiteral("resolveProvider")).toBool();
    if (canResolve && !json.contains(QStringLiteral("edit")) && !json.value(QStringLiteral("command")).isObject() &&
        json.contains(QStringLiteral("data"))) {
        QPointer<LspClient> guard(c);
        QPointer<Document> docGuard(doc);
        c->request(QStringLiteral("codeAction/resolve"), json, [this, guard, docGuard, action, done](const QJsonValue &result, const QJsonObject &error) {
            if (!guard || !docGuard || !error.isEmpty() || !result.isObject()) {
                done(false);
                return;
            }
            LspCodeAction resolved = action;
            resolved.json = result.toObject();
            resolved.json.remove(QStringLiteral("data")); // resolved once
            if (!resolved.json.contains(QStringLiteral("edit")) && !resolved.json.value(QStringLiteral("command")).isObject()) {
                done(false);
                return;
            }
            runCodeAction(docGuard, resolved, done);
        });
        return;
    }
    bool ok = true;
    const QJsonObject edit = json.value(QStringLiteral("edit")).toObject();
    if (!edit.isEmpty())
        ok = m_applyEdit && m_applyEdit(edit);
    const QJsonValue command = json.value(QStringLiteral("command"));
    // CodeAction.command is a Command object; a bare Command (the whole action) carries a string.
    QJsonObject cmd = command.isObject() ? command.toObject() : QJsonObject();
    if (cmd.isEmpty() && command.isString())
        cmd = json;
    if (cmd.isEmpty()) {
        done(ok);
        return;
    }
    // jdtls offers some fixes as a client-side command that carries the edit; it is not a server command.
    if (cmd.value(QStringLiteral("command")).toString() == QLatin1String("java.apply.workspaceEdit")) {
        bool applied = ok;
        for (const QJsonValue &e : cmd.value(QStringLiteral("arguments")).toArray())
            applied = (m_applyEdit && m_applyEdit(e.toObject())) && applied;
        done(applied);
        return;
    }
    c->request(QStringLiteral("workspace/executeCommand"),
               QJsonObject{{QStringLiteral("command"), cmd.value(QStringLiteral("command"))}, {QStringLiteral("arguments"), cmd.value(QStringLiteral("arguments"))}},
               [done, ok](const QJsonValue &, const QJsonObject &err) { done(ok && err.isEmpty()); });
}

// Source actions (organize imports, fix all ...): for each kind, every server holding the file is asked in turn (main server
// first) and what it offers is run before the next request, so each action sees the text the previous one produced.
void LspManager::applySourceActions(Document *doc, const QStringList &kinds, std::function<void(int, int)> done, std::shared_ptr<bool> cancel)
{
    const auto tracked = m_tracked.constFind(doc);
    if (tracked == m_tracked.constEnd() || !tracked->opened()) {
        done(0, 0);
        return;
    }
    struct Step {
        QString kind, server;
    };
    struct Run {
        QVector<Step> steps;
        int next = 0, applied = 0, found = 0;
        std::function<void()> step;
    };
    auto run = std::make_shared<Run>();
    for (const QString &kind : kinds)
        for (const QString &id : tracked->servers())
            if (tracked->openOn.contains(id))
                run->steps.append({kind, id});
    QPointer<Document> guard(doc);
    run->step = [this, run, guard, done, cancel] {
        if (!guard || (cancel && *cancel) || run->next >= run->steps.size()) {
            auto finish = done;
            const int applied = run->applied, found = run->found;
            run->step = nullptr; // break the cycle
            finish(applied, found);
            return;
        }
        const Step st = run->steps.at(run->next++);
        const QTextDocument *td = guard->textDocument();
        const int lastLine = qMax(0, td->blockCount() - 1);
        const int lastColumn = td->lastBlock().isValid() ? qMax(0, td->lastBlock().length() - 1) : 0;
        codeActions(guard, 0, 0, lastLine, lastColumn, [this, run, guard, st, cancel](const QVector<LspCodeAction> &found) {
            QVector<LspCodeAction> wanted;
            for (const LspCodeAction &a : found)
                if (a.kind == st.kind || a.kind.startsWith(st.kind + QLatin1Char('.')))
                    wanted << a;
            run->found += int(wanted.size());
            // Actions of one answer are alternatives computed from the same text; the first is enough, and running a
            // second would work from positions the first already moved.
            if (wanted.isEmpty() || !guard || (cancel && *cancel)) {
                QTimer::singleShot(0, [run] { if (run->step) run->step(); });
                return;
            }
            runCodeAction(guard, wanted.first(), [run](bool ok) {
                if (ok)
                    ++run->applied;
                QTimer::singleShot(0, [run] { if (run->step) run->step(); });
            });
        }, {st.kind}, false, st.server);
    };
    run->step();
}

int LspManager::applySourceActionsBlocking(Document *doc, const QStringList &kinds, int timeoutMs)
{
    struct State {
        bool finished = false;
        int applied = 0;
        QEventLoop loop;
    };
    auto st = std::make_shared<State>();
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &st->loop, &QEventLoop::quit);
    auto cancel = std::make_shared<bool>(false);
    applySourceActions(doc, kinds, [st](int applied, int) {
        st->finished = true;
        st->applied = applied;
        st->loop.quit();
    }, cancel);
    if (!st->finished) {
        timeout.start(timeoutMs);
        st->loop.exec();
    }
    *cancel = true; // a step still in flight must not edit the document after the save
    return st->applied;
}

// --- Signature help / highlights / inlay hints -----------------------------------------------------

bool LspManager::signatureTriggers(Document *doc, QString *triggers, QString *retriggers) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd())
        return false;
    for (const QString &id : it->servers()) {
        const Server &s = m_servers[id];
        if (!it->openOn.contains(id) || !s.client || !s.client->isRunning())
            continue;
        const QJsonValue p = s.client->serverCapabilities().value(QStringLiteral("signatureHelpProvider"));
        if (!p.isObject())
            continue;
        auto chars = [&p](const char *key) {
            QString out;
            for (const QJsonValue &v : p.toObject().value(QLatin1String(key)).toArray())
                out += v.toString();
            return out;
        };
        *triggers = chars("triggerCharacters");
        *retriggers = chars("retriggerCharacters");
        return true;
    }
    return false;
}

static QString signatureDoc(const QJsonValue &v)
{
    return markupText(v).trimmed();
}

void LspManager::signatureHelp(Document *doc, int line, int column, int triggerKind, const QString &triggerChar, bool retrigger,
                               std::function<void(const LspSignatureHelp &)> done)
{
    QString uri;
    LspClient *c = nullptr;
    for (LspClient *candidate : readyClientsFor(doc, &uri))
        if (candidate->serverCapabilities().value(QStringLiteral("signatureHelpProvider")).isObject()) {
            c = candidate;
            break;
        }
    if (!c) {
        done({});
        return;
    }
    if (m_signatureClient && m_signatureRequest)
        m_signatureClient->cancel(m_signatureRequest);
    QJsonObject params = positionParams(uri, line, column);
    QJsonObject context{{QStringLiteral("triggerKind"), triggerKind}, {QStringLiteral("isRetrigger"), retrigger}};
    if (triggerKind == 2 && !triggerChar.isEmpty())
        context.insert(QStringLiteral("triggerCharacter"), triggerChar);
    params.insert(QStringLiteral("context"), context);
    m_signatureClient = c;
    m_signatureRequest = c->request(QStringLiteral("textDocument/signatureHelp"), params, [done](const QJsonValue &result, const QJsonObject &error) {
        LspSignatureHelp out;
        if (error.isEmpty() && result.isObject()) {
            const QJsonObject o = result.toObject();
            out.activeSignature = o.value(QStringLiteral("activeSignature")).toInt(0);
            out.activeParameter = o.value(QStringLiteral("activeParameter")).toInt(-1);
            for (const QJsonValue &sv : o.value(QStringLiteral("signatures")).toArray()) {
                const QJsonObject so = sv.toObject();
                LspSignature sig;
                sig.label = so.value(QStringLiteral("label")).toString();
                sig.documentation = signatureDoc(so.value(QStringLiteral("documentation")));
                sig.active = so.contains(QStringLiteral("activeParameter")) ? so.value(QStringLiteral("activeParameter")).toInt(-1) : -1;
                for (const QJsonValue &pv : so.value(QStringLiteral("parameters")).toArray()) {
                    const QJsonObject po = pv.toObject();
                    const QJsonValue label = po.value(QStringLiteral("label"));
                    QPair<int, int> range{-1, -1};
                    if (label.isArray() && label.toArray().size() == 2) {
                        range = {label.toArray().at(0).toInt(), label.toArray().at(1).toInt()};
                    } else if (label.isString()) {
                        const int at = sig.label.indexOf(label.toString());
                        if (at >= 0)
                            range = {at, at + int(label.toString().size())};
                    }
                    sig.params << range;
                    sig.paramDocs << signatureDoc(po.value(QStringLiteral("documentation")));
                }
                out.signatures << sig;
            }
        }
        done(out);
    });
}

bool LspManager::supportsHighlights(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const Server &s = m_servers[it->serverId];
    if (!s.client || !s.client->isRunning())
        return false;
    const QJsonValue v = s.client->serverCapabilities().value(QStringLiteral("documentHighlightProvider"));
    return v.isObject() || v.toBool(false);
}

void LspManager::documentHighlights(Document *doc, int line, int column, std::function<void(const QVector<LspHighlight> &)> done)
{
    QString uri;
    LspClient *c = supportsHighlights(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({});
        return;
    }
    c->request(QStringLiteral("textDocument/documentHighlight"), positionParams(uri, line, column), [done](const QJsonValue &result, const QJsonObject &error) {
        QVector<LspHighlight> out;
        if (error.isEmpty() && result.isArray())
            for (const QJsonValue &v : result.toArray()) {
                const QJsonObject o = v.toObject();
                const QJsonObject r = o.value(QStringLiteral("range")).toObject();
                const QJsonObject a = r.value(QStringLiteral("start")).toObject(), b = r.value(QStringLiteral("end")).toObject();
                LspHighlight h;
                h.startLine = a.value(QStringLiteral("line")).toInt();
                h.startColumn = a.value(QStringLiteral("character")).toInt();
                h.endLine = b.value(QStringLiteral("line")).toInt();
                h.endColumn = b.value(QStringLiteral("character")).toInt();
                h.kind = o.value(QStringLiteral("kind")).toInt(1);
                out << h;
            }
        done(out);
    });
}

bool LspManager::supportsInlayHints(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd())
        return false;
    for (const QString &id : it->servers()) {
        const Server &s = m_servers[id];
        if (!it->openOn.contains(id) || !s.client || !s.client->isRunning())
            continue;
        const QJsonValue v = s.client->serverCapabilities().value(QStringLiteral("inlayHintProvider"));
        if (v.isObject() || v.toBool(false))
            return true;
    }
    return false;
}

void LspManager::inlayHints(Document *doc, int startLine, int endLine, std::function<void(const QVector<LspInlayHint> &)> done)
{
    QString uri;
    LspClient *c = nullptr;
    for (LspClient *candidate : readyClientsFor(doc, &uri)) {
        const QJsonValue v = candidate->serverCapabilities().value(QStringLiteral("inlayHintProvider"));
        if (v.isObject() || v.toBool(false)) {
            c = candidate;
            break;
        }
    }
    if (!c) {
        done({});
        return;
    }
    const QTextDocument *td = doc->textDocument();
    const QTextBlock last = td->findBlockByNumber(qMin(endLine, td->blockCount() - 1));
    const QJsonObject range{{QStringLiteral("start"), QJsonObject{{QStringLiteral("line"), startLine}, {QStringLiteral("character"), 0}}},
                            {QStringLiteral("end"), QJsonObject{{QStringLiteral("line"), last.isValid() ? last.blockNumber() : endLine},
                                                                {QStringLiteral("character"), last.isValid() ? last.length() - 1 : 0}}}};
    c->request(QStringLiteral("textDocument/inlayHint"),
               QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}, {QStringLiteral("range"), range}},
               [done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspInlayHint> out;
                   if (error.isEmpty() && result.isArray())
                       for (const QJsonValue &v : result.toArray()) {
                           const QJsonObject o = v.toObject();
                           const QJsonObject pos = o.value(QStringLiteral("position")).toObject();
                           LspInlayHint h;
                           h.line = pos.value(QStringLiteral("line")).toInt();
                           h.column = pos.value(QStringLiteral("character")).toInt();
                           const QJsonValue label = o.value(QStringLiteral("label"));
                           if (label.isString()) {
                               h.label = label.toString();
                           } else {
                               for (const QJsonValue &part : label.toArray())
                                   h.label += part.toObject().value(QStringLiteral("value")).toString();
                           }
                           h.kind = o.value(QStringLiteral("kind")).toInt(0);
                           h.padLeft = o.value(QStringLiteral("paddingLeft")).toBool();
                           h.padRight = o.value(QStringLiteral("paddingRight")).toBool();
                           // The Kotlin server sends one hint per part at the same spot ("this: " + type, spaces around):
                           // they become a single pill.
                           if (!out.isEmpty() && out.last().line == h.line && out.last().column == h.column) {
                               out.last().label += h.label;
                               out.last().padRight = out.last().padRight || h.padRight;
                               continue;
                           }
                           out << h;
                       }
                   for (LspInlayHint &h : out)
                       h.label = h.label.trimmed();
                   out.erase(std::remove_if(out.begin(), out.end(), [](const LspInlayHint &h) { return h.label.isEmpty(); }), out.end());
                   done(out);
               });
}

// typescript-language-server computes hints only for the kinds its preferences allow, and they are read when it starts (a
// later didChangeConfiguration is ignored). So every kind is switched on and the editor filters what it shows: toggling
// a hint class in QODE needs no server restart, and nothing is computed until hints are requested.
QJsonObject LspManager::tsPreferences() const
{
    return {{QStringLiteral("includeInlayParameterNameHints"), QStringLiteral("all")},
            {QStringLiteral("includeInlayParameterNameHintsWhenArgumentMatchesName"), false},
            {QStringLiteral("includeInlayFunctionParameterTypeHints"), true},
            {QStringLiteral("includeInlayVariableTypeHints"), true},
            {QStringLiteral("includeInlayVariableTypeHintsWhenTypeMatchesName"), false},
            {QStringLiteral("includeInlayPropertyDeclarationTypeHints"), true},
            {QStringLiteral("includeInlayFunctionLikeReturnTypeHints"), true},
            {QStringLiteral("includeInlayEnumMemberValueHints"), true}};
}

// Documentation comment and declaration found in a source file at a definition's line: the fallback for servers whose hover
// is empty for a symbol they can still navigate to (the Kotlin server does this for Compose's Button, Switch, Card ...).
static QString sourceSummary(const QString &path, int line)
{
    if (!path.endsWith(QLatin1String(".kt")) && !path.endsWith(QLatin1String(".java")))
        return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    if (line < 0 || line >= lines.size())
        return {};

    // Annotations and modifiers on lines above the name belong to the declaration.
    int start = line;
    while (start > 0) {
        const QString prev = lines[start - 1].trimmed();
        if (prev.startsWith(QLatin1Char('@')) || (prev.endsWith(QLatin1Char(',')) && start - 1 > 0 && lines[start - 2].trimmed().startsWith(QLatin1Char('@'))))
            --start;
        else
            break;
    }
    // Signature: up to the body ('{' or '=' outside the parentheses) or a limit.
    QStringList sig;
    int depth = 0;
    bool done = false;
    for (int i = start; i < lines.size() && i < start + 40 && !done; ++i) {
        const QString &l = lines[i];
        QString kept;
        for (int k = 0; k < l.size() && !done; ++k) {
            const QChar ch = l[k];
            if (ch == QLatin1Char('(') || ch == QLatin1Char('<'))
                ++depth;
            else if (ch == QLatin1Char(')') || (ch == QLatin1Char('>') && k > 0 && l[k - 1] != QLatin1Char('-')))
                --depth;
            else if (depth <= 0 && (ch == QLatin1Char('{') || (ch == QLatin1Char('=') && k + 1 < l.size() && l[k + 1] != QLatin1Char('=')) ))
                done = true;
            if (!done)
                kept += ch;
        }
        sig << (kept.trimmed().isEmpty() ? QString() : kept);
        if (!done && depth <= 0 && i >= line && !kept.trimmed().isEmpty() && !kept.trimmed().endsWith(QLatin1Char(',')) && !kept.trimmed().endsWith(QLatin1Char('(')))
            done = true;
    }
    while (!sig.isEmpty() && sig.last().trimmed().isEmpty())
        sig.removeLast();
    if (sig.isEmpty())
        return {};

    // KDoc / Javadoc ending right above.
    QStringList doc;
    int e = start - 1;
    if (e >= 0 && lines[e].trimmed().endsWith(QLatin1String("*/"))) {
        int b = e;
        while (b > 0 && !lines[b].contains(QLatin1String("/*")))
            --b;
        for (int i = b; i <= e; ++i) {
            QString t = lines[i].trimmed();
            if (t.startsWith(QLatin1String("/**")))
                t = t.mid(3);
            else if (t.startsWith(QLatin1String("/*")))
                t = t.mid(2);
            if (t.endsWith(QLatin1String("*/")))
                t.chop(2);
            if (t.startsWith(QLatin1Char('*')))
                t = t.mid(1);
            t = t.trimmed();
            if (t.startsWith(QLatin1String("@sample")) || t.startsWith(QLatin1String("@see")) || t.startsWith(QLatin1String("@suppress")))
                continue;
            if (t.startsWith(QLatin1String("@param ")) || t.startsWith(QLatin1String("@property ")))
                t = QStringLiteral("- `") + t.section(QLatin1Char(' '), 1, 1) + QStringLiteral("`: ") + t.section(QLatin1Char(' '), 2);
            else if (t.startsWith(QLatin1String("@return")))
                t = QStringLiteral("**Returns** ") + t.mid(7).trimmed();
            doc << t;
        }
        while (!doc.isEmpty() && doc.first().isEmpty())
            doc.removeFirst();
        while (!doc.isEmpty() && doc.last().isEmpty())
            doc.removeLast();
    }

    // Common indentation off the signature.
    int indent = 1 << 20;
    for (const QString &l : std::as_const(sig))
        if (!l.trimmed().isEmpty()) {
            int n = 0;
            while (n < l.size() && l[n].isSpace())
                ++n;
            indent = qMin(indent, n);
        }
    for (QString &l : sig)
        l = l.mid(qMin(indent, int(l.size())));

    QString out = doc.join(QLatin1Char('\n'));
    out.remove(QRegularExpression(QStringLiteral(R"(!\[[^\]]*\]\([^)]*\))")));
    if (!out.isEmpty())
        out += QStringLiteral("\n\n");
    return out + QStringLiteral("```") + (path.endsWith(QLatin1String(".kt")) ? QStringLiteral("kotlin") : QStringLiteral("java")) + QLatin1Char('\n')
           + sig.join(QLatin1Char('\n')) + QStringLiteral("\n```");
}

void LspManager::hover(Document *doc, int line, int column, std::function<void(const QString &)> done)
{
    QString uri;
    QList<LspClient *> clients;
    for (LspClient *c : readyClientsFor(doc, &uri))
        if (c->serverCapabilities().value(QStringLiteral("hoverProvider")) != false && c->serverCapabilities().contains(QStringLiteral("hoverProvider")))
            clients << c;
    if (clients.isEmpty()) {
        done({});
        return;
    }
    // Main server first, then companions (Tailwind shows the CSS a class generates): join what they say.
    struct Gather {
        int pending = 0;
        QStringList texts; // by client order
    };
    auto gather = std::make_shared<Gather>();
    gather->pending = clients.size();
    gather->texts = QStringList(clients.size());
    for (int i = 0; i < clients.size(); ++i)
        clients[i]->request(QStringLiteral("textDocument/hover"), positionParams(uri, line, column),
                            [this, gather, done, i, docGuard = QPointer<Document>(doc), line, column](const QJsonValue &result, const QJsonObject &error) {
                                if (error.isEmpty() && result.isObject())
                                    gather->texts[i] = markupText(result.toObject().value(QStringLiteral("contents"))).trimmed();
                                if (--gather->pending == 0) {
                                    QStringList parts;
                                    for (const QString &t : std::as_const(gather->texts))
                                        if (!t.isEmpty())
                                            parts << t;
                                    if (!parts.isEmpty()) {
                                        done(parts.join(QStringLiteral("\n\n")));
                                        return;
                                    }
                                    // Nothing from any server: document what the definition points at.
                                    if (!docGuard) {
                                        done({});
                                        return;
                                    }
                                    definition(docGuard, line, column, [done](const QVector<LspLocation> &where) {
                                        done(where.isEmpty() ? QString() : sourceSummary(where.first().path, where.first().line));
                                    });
                                }
                            });
}

bool LspManager::supportsNavigation(Document *doc, Navigate what) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    if (!c || !c->isRunning())
        return false;
    const char *key = what == Navigate::TypeDefinition ? "typeDefinitionProvider"
                      : what == Navigate::Implementation ? "implementationProvider" : "definitionProvider";
    const QJsonValue p = c->serverCapabilities().value(QLatin1String(key));
    return p.isObject() || p.toBool(false);
}

void LspManager::switchSourceHeader(Document *doc, std::function<void(const QString &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c || m_tracked.value(doc).serverId != QLatin1String("clangd")) {
        done({});
        return;
    }
    c->request(QStringLiteral("textDocument/switchSourceHeader"), QJsonObject{{QStringLiteral("uri"), uri}},
               [done](const QJsonValue &result, const QJsonObject &error) {
                   done(error.isEmpty() && result.isString() ? QUrl(result.toString()).toLocalFile() : QString());
               });
}

void LspManager::definition(Document *doc, int line, int column, std::function<void(const QVector<LspLocation> &)> done, Navigate what)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    QPointer<LspClient> guard(c);
    const QString method = what == Navigate::TypeDefinition ? QStringLiteral("textDocument/typeDefinition")
                           : what == Navigate::Implementation ? QStringLiteral("textDocument/implementation")
                                                              : QStringLiteral("textDocument/definition");
    c->request(method, positionParams(uri, line, column),
               [guard, done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<QJsonObject> found;
                   if (error.isEmpty()) {
                       if (result.isArray())
                           for (const QJsonValue &v : result.toArray())
                               found << v.toObject();
                       else if (result.isObject())
                           found << result.toObject();
                   }
                   // jdtls answers "jdt://contents/…class" for library classes: ask it for the text, which is cached as a file.
                   struct Gather {
                       int pending = 0;
                       QVector<LspLocation> out;
                   };
                   auto gather = std::make_shared<Gather>();
                   QVector<QJsonObject> classFiles;
                   for (const QJsonObject &o : std::as_const(found)) {
                       const QString target = o.value(o.contains(QStringLiteral("targetUri")) ? QStringLiteral("targetUri") : QStringLiteral("uri")).toString();
                       if (target.startsWith(QLatin1String("jdt:")) && guard)
                           classFiles << o;
                       else
                           addLocation(gather->out, o);
                   }
                   if (classFiles.isEmpty()) {
                       done(gather->out);
                       return;
                   }
                   gather->pending = classFiles.size();
                   for (const QJsonObject &o : std::as_const(classFiles)) {
                       const bool link = o.contains(QStringLiteral("targetUri"));
                       const QString target = o.value(link ? QStringLiteral("targetUri") : QStringLiteral("uri")).toString();
                       const QJsonObject start = o.value(link ? QStringLiteral("targetSelectionRange") : QStringLiteral("range")).toObject().value(QStringLiteral("start")).toObject();
                       guard->request(QStringLiteral("java/classFileContents"), QJsonObject{{QStringLiteral("uri"), target}},
                                      [gather, done, target, start](const QJsonValue &text, const QJsonObject &err) {
                                          if (err.isEmpty() && text.isString()) {
                                              LspLocation loc;
                                              loc.path = JarSource::storeText(target, text.toString().toUtf8());
                                              loc.line = start.value(QStringLiteral("line")).toInt();
                                              loc.column = start.value(QStringLiteral("character")).toInt();
                                              if (!loc.path.isEmpty())
                                                  gather->out << loc;
                                          }
                                          if (--gather->pending == 0)
                                              done(gather->out);
                                      });
                   }
               });
}

// --- Server -> client ------------------------------------------------------------------------------

void LspManager::onNotification(const QString &id, const QString &method, const QJsonValue &params)
{
    if (method == QLatin1String("$/progress")) {
        // Work-done progress: "begin" / "report" / "end" for a token. The newest running operation is shown.
        Server &s = m_servers[id];
        const QJsonObject o = params.toObject();
        const QString token = o.value(QStringLiteral("token")).toVariant().toString();
        const QJsonObject v = o.value(QStringLiteral("value")).toObject();
        const QString kind = v.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("end")) {
            s.progress.remove(token);
        } else {
            Server::Progress &p = s.progress[token];
            const QString title = v.value(QStringLiteral("title")).toString();
            if (!title.isEmpty())
                p.title = title; // "report" messages omit it
            // A report without a message keeps the last one; one without a percentage too.
            if (v.contains(QStringLiteral("message")) || kind == QLatin1String("begin"))
                p.text = v.value(QStringLiteral("message")).toString();
            if (v.contains(QStringLiteral("percentage")))
                p.percent = qBound(0, v.value(QStringLiteral("percentage")).toInt(), 100);
            p.order = ++m_progressOrder;
        }
        updateProgressState(s);
        progressChanged(s);
        scheduleStatusChanged();
        return;
    }
    if (method == QLatin1String("language/progressReport")) {
        // jdtls' own progress notification: {id, task, subTask, status, totalWork, workDone, complete}.
        Server &s = m_servers[id];
        const QJsonObject o = params.toObject();
        const QString token = o.value(QStringLiteral("id")).toString();
        if (o.value(QStringLiteral("complete")).toBool()) {
            s.progress.remove(token);
        } else {
            const QString task = o.value(QStringLiteral("task")).toString();
            Server::Progress &p = s.progress[token];
            if (task.contains(QStringLiteral("Gradle")) || task.contains(QStringLiteral("Maven")) || task.contains(QStringLiteral("mport"))
                || task.contains(QStringLiteral("ynchroni")))
                p.title = task.contains(QStringLiteral("Maven")) ? tr("Syncing Maven project") : tr("Syncing Gradle project");
            else if (task.startsWith(QLatin1String("Search")))
                p.title = tr("Indexing");
            else
                p.title = QString(task).remove(QStringLiteral("...")).trimmed();
            // subTask / status name the step ("Synchronizing projects", "Building workspace"): shown after the title.
            QString step = o.value(QStringLiteral("subTask")).toString().trimmed();
            if (step.isEmpty())
                step = o.value(QStringLiteral("status")).toString().trimmed();
            step.remove(QStringLiteral("..."));
            static const QRegularExpression barePercent(QStringLiteral(R"(^\d+\s*%$)"));
            if (barePercent.match(step).hasMatch())
                step.clear(); // "0%": the percentage comes from workDone / totalWork (and is left out for indexing)
            p.text = step == task || step == p.title ? QString() : step.left(80);
            const int total = o.value(QStringLiteral("totalWork")).toInt();
            // The search index reports a total that keeps growing: no percentage for it.
            p.percent = total > 0 && p.title != tr("Indexing") ? qBound(0, o.value(QStringLiteral("workDone")).toInt() * 100 / total, 100) : -1;
            p.order = ++m_progressOrder;
        }
        updateProgressState(s);
        progressChanged(s);
        scheduleStatusChanged();
        return;
    }
    if (method == QLatin1String("workspace/diagnostic/refresh")) {
        // Indexing sends these in bursts: pull once the burst is over.
        Server &s = m_servers[id];
        const int gen = ++s.refreshGen;
        QTimer::singleShot(800, this, [this, id, gen] {
            if (m_servers.value(id).refreshGen != gen)
                return;
            for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it)
                if (it->servers().contains(id) && it->openOn.contains(id))
                    pullDiagnostics(it.key());
        });
        return;
    }
    if (method == QLatin1String("workspace/inlayHint/refresh")) {
        emit inlayHintsRefresh();
        return;
    }
    if (method == QLatin1String("workspace/semanticTokens/refresh") || method == QLatin1String("workspace/foldingRange/refresh") ||
        method == QLatin1String("workspace/codeLens/refresh")) {
        emit serverDataRefresh();
        return;
    }
    if (method == QLatin1String("_typescript.rename")) {
        const QJsonObject o = params.toObject();
        const QJsonObject pos = o.value(QStringLiteral("position")).toObject();
        emit renameRequested(QUrl(o.value(QStringLiteral("textDocument")).toObject().value(QStringLiteral("uri")).toString()).toLocalFile(),
                             pos.value(QStringLiteral("line")).toInt(), pos.value(QStringLiteral("character")).toInt());
        return;
    }
    if (method != QLatin1String("textDocument/publishDiagnostics"))
        return;
    const QJsonObject o = params.toObject();
    const QString path = QUrl(o.value(QStringLiteral("uri")).toString()).toLocalFile();
    setDiagnosticsFor(path, o.value(QStringLiteral("diagnostics")).toArray(), id);
    noteReported(path, id, o.value(QStringLiteral("version")).toInt(-1));
}

// A report without a version (optional in publishDiagnostics) is taken as one for the text the server has now.
void LspManager::noteReported(const QString &path, const QString &serverId, int version)
{
    for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it) {
        if (it->path != path || it->serverId != serverId)
            continue;
        it->reported = qMax(it->reported, version < 0 ? it->version : version);
        emit diagnosticsReported(path);
        return;
    }
}

int LspManager::syncNow(Document *doc)
{
    auto it = m_tracked.find(doc);
    if (it == m_tracked.end() || !it->opened())
        return -1;
    m_changeTimer->stop();
    flushChanges();
    it = m_tracked.find(doc);
    if (it == m_tracked.end())
        return -1;
    if (it->reported < it->version)
        pullDiagnostics(doc); // a pull server's answer to an earlier request may have been an error: ask again
    return it->version;
}

int LspManager::diagnosticsVersion(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    return it == m_tracked.constEnd() ? -1 : it->reported;
}

void LspManager::setDiagnosticsFor(const QString &path, const QJsonArray &items, const QString &serverId)
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
        diag.server = serverId;
        diag.raw = d;
        if (serverId == QLatin1String("java"))
            for (auto it = m_tracked.constBegin(); it != m_tracked.constEnd(); ++it)
                if (it->path == path) {
                    if (missingNew(it.key()->textDocument(), diag.startLine, diag.startColumn, diag.message))
                        diag.message += tr("\n\nDid you forget 'new'? Press Alt+Enter to add it.");
                    break;
                }
        list.append(diag);
    }
    const QVector<LspDiagnostic> before = m_diagBySource.value(path).value(serverId);
    bool same = before.size() == list.size();
    for (int i = 0; same && i < list.size(); ++i)
        same = before[i].raw == list[i].raw && before[i].message == list[i].message;
    if (same)
        return; // an indexing server repeats itself: nothing to repaint
    if (list.isEmpty()) {
        m_diagBySource[path].remove(serverId);
        if (m_diagBySource[path].isEmpty())
            m_diagBySource.remove(path);
    } else {
        m_diagBySource[path].insert(serverId, list);
    }
    publishMerged(path);
}

// The editor shows one list per file: what every server reported, main server first.
void LspManager::publishMerged(const QString &path)
{
    const auto &bySource = m_diagBySource.value(path);
    QVector<LspDiagnostic> merged;
    QStringList ids = bySource.keys();
    const auto tracked = std::find_if(m_tracked.constBegin(), m_tracked.constEnd(), [&path](const Tracked &t) { return t.path == path; });
    if (tracked != m_tracked.constEnd() && ids.contains(tracked->serverId)) {
        ids.removeAll(tracked->serverId);
        ids.prepend(tracked->serverId);
    }
    for (const QString &id : std::as_const(ids))
        merged += bySource.value(id);
    if (merged.isEmpty())
        m_diagnostics.remove(path);
    else
        m_diagnostics.insert(path, merged);
    emit diagnosticsChanged(path);
    emit statusChanged();
}

// Pull model: the server publishes nothing on its own, so ask after every open / change / save.
void LspManager::pullDiagnostics(Document *doc)
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd())
        return;
    for (const QString &id : it->servers()) {
        if (!it->openOn.contains(id))
            continue;
        LspClient *client = m_servers.value(id).client;
        if (!client || !client->isRunning() || !client->serverCapabilities().contains(QStringLiteral("diagnosticProvider")))
            continue;
        const QString path = it->path;
        const int version = it->version; // the answer describes the text the server had when asked
        QPointer<LspClient> guard(client);
        client->request(QStringLiteral("textDocument/diagnostic"),
                        QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), it->uri}}}},
                        [this, path, guard, id, version](const QJsonValue &result, const QJsonObject &error) {
                            if (!guard || !error.isEmpty())
                                return;
                            const QJsonObject r = result.toObject();
                            if (r.value(QStringLiteral("kind")).toString() == QLatin1String("full"))
                                setDiagnosticsFor(path, r.value(QStringLiteral("items")).toArray(), id);
                            // A pull is answered at once, also while the server is still importing the project (Kotlin
                            // then returns nothing and asks for a refresh later): only an answer from an idle server
                            // counts as its verdict on this version.
                            if (m_servers.value(id).progress.isEmpty())
                                noteReported(path, id, version);
                        });
    }
}

void LspManager::clearDiagnosticsFor(const QString &path, const QString &serverId)
{
    auto it = m_diagBySource.find(path);
    if (it == m_diagBySource.end())
        return;
    if (serverId.isEmpty())
        m_diagBySource.erase(it);
    else {
        it->remove(serverId);
        if (it->isEmpty())
            m_diagBySource.erase(it);
    }
    publishMerged(path);
}

// --- Colours, rename, references -----------------------------------------------------------------

void LspManager::documentLinks(Document *doc, std::function<void(const QVector<LspDocumentLink> &)> done)
{
    QString uri;
    QList<LspClient *> clients;
    for (LspClient *c : readyClientsFor(doc, &uri))
        if (c->serverCapabilities().value(QStringLiteral("documentLinkProvider")).isObject())
            clients << c;
    if (clients.isEmpty()) {
        done({});
        return;
    }
    struct Gather {
        int pending = 0;
        QVector<LspDocumentLink> links;
    };
    auto gather = std::make_shared<Gather>();
    gather->pending = int(clients.size());
    for (LspClient *c : clients)
        c->request(QStringLiteral("textDocument/documentLink"), QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}},
                   [gather, done](const QJsonValue &result, const QJsonObject &error) {
                       if (error.isEmpty())
                           for (const QJsonValue &v : result.toArray()) {
                               const QJsonObject o = v.toObject();
                               const QString target = o.value(QStringLiteral("target")).toString();
                               if (target.isEmpty())
                                   continue;
                               const QJsonObject r = o.value(QStringLiteral("range")).toObject();
                               const QJsonObject a = r.value(QStringLiteral("start")).toObject(), b = r.value(QStringLiteral("end")).toObject();
                               LspDocumentLink l;
                               l.startLine = a.value(QStringLiteral("line")).toInt();
                               l.startColumn = a.value(QStringLiteral("character")).toInt();
                               l.endLine = b.value(QStringLiteral("line")).toInt();
                               l.endColumn = b.value(QStringLiteral("character")).toInt();
                               l.target = target;
                               gather->links.append(l);
                           }
                       if (--gather->pending == 0)
                           done(gather->links);
                   });
}

void LspManager::documentColors(Document *doc, std::function<void(const QVector<LspColor> &)> done)
{
    QString uri;
    QList<LspClient *> clients;
    for (LspClient *c : readyClientsFor(doc, &uri))
        if (c->serverCapabilities().contains(QStringLiteral("colorProvider")) && c->serverCapabilities().value(QStringLiteral("colorProvider")) != false)
            clients << c;
    if (clients.isEmpty()) {
        done({});
        return;
    }
    struct Gather {
        int pending = 0;
        QVector<LspColor> colors;
    };
    auto gather = std::make_shared<Gather>();
    gather->pending = clients.size();
    for (LspClient *c : clients) {
        QString serverId;
        for (auto it = m_servers.constBegin(); it != m_servers.constEnd(); ++it)
            if (it->client == c)
                serverId = it.key();
        c->request(QStringLiteral("textDocument/documentColor"), QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}},
                   [gather, done, serverId](const QJsonValue &result, const QJsonObject &error) {
                       if (error.isEmpty())
                           for (const QJsonValue &v : result.toArray()) {
                               const QJsonObject o = v.toObject();
                               const QJsonObject r = o.value(QStringLiteral("range")).toObject();
                               const QJsonObject a = r.value(QStringLiteral("start")).toObject(), b = r.value(QStringLiteral("end")).toObject();
                               const QJsonObject col = o.value(QStringLiteral("color")).toObject();
                               LspColor lc;
                               lc.startLine = a.value(QStringLiteral("line")).toInt();
                               lc.startColumn = a.value(QStringLiteral("character")).toInt();
                               lc.endLine = b.value(QStringLiteral("line")).toInt();
                               lc.endColumn = b.value(QStringLiteral("character")).toInt();
                               lc.red = col.value(QStringLiteral("red")).toDouble();
                               lc.green = col.value(QStringLiteral("green")).toDouble();
                               lc.blue = col.value(QStringLiteral("blue")).toDouble();
                               lc.alpha = col.value(QStringLiteral("alpha")).toDouble(1);
                               lc.raw = o;
                               lc.server = serverId;
                               gather->colors.append(lc);
                           }
                       if (--gather->pending == 0)
                           done(gather->colors);
                   });
    }
}

void LspManager::colorPresentations(Document *doc, const LspColor &color, const QColor &picked,
                                    std::function<void(const QVector<LspColorPresentation> &)> done)
{
    QString uri;
    LspClient *c = nullptr;
    for (LspClient *candidate : readyClientsFor(doc, &uri))
        if (m_servers.value(color.server).client == candidate)
            c = candidate;
    if (!c) {
        done({});
        return;
    }
    const QJsonObject col{{QStringLiteral("red"), picked.redF()}, {QStringLiteral("green"), picked.greenF()},
                          {QStringLiteral("blue"), picked.blueF()}, {QStringLiteral("alpha"), picked.alphaF()}};
    c->request(QStringLiteral("textDocument/colorPresentation"),
               QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}},
                           {QStringLiteral("color"), col},
                           {QStringLiteral("range"), color.raw.value(QStringLiteral("range"))}},
               [done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspColorPresentation> out;
                   if (error.isEmpty())
                       for (const QJsonValue &v : result.toArray()) {
                           const QJsonObject o = v.toObject();
                           LspColorPresentation p;
                           p.label = o.value(QStringLiteral("label")).toString();
                           if (o.contains(QStringLiteral("textEdit"))) {
                               p.edit = parseEdit(o.value(QStringLiteral("textEdit")).toObject());
                               p.hasEdit = true;
                           }
                           out << p;
                       }
                   done(out);
               });
}

bool LspManager::supportsRename(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    if (!c || !c->isRunning())
        return false;
    const QJsonValue p = c->serverCapabilities().value(QStringLiteral("renameProvider"));
    return !p.isUndefined() && !p.isNull() && p != false;
}

void LspManager::prepareRename(Document *doc, int line, int column, std::function<void(bool, const QString &, const QString &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done(false, {}, tr("No language server is running for this file"));
        return;
    }
    const QJsonValue provider = c->serverCapabilities().value(QStringLiteral("renameProvider"));
    const bool prepare = provider.isObject() && provider.toObject().value(QStringLiteral("prepareProvider")).toBool();
    // Without prepareRename the caller falls back to the word under the cursor.
    if (!prepare) {
        done(true, {}, {});
        return;
    }
    const QString text = doc->text();
    c->request(QStringLiteral("textDocument/prepareRename"), positionParams(uri, line, column),
               [done, text](const QJsonValue &result, const QJsonObject &error) {
                   if (!error.isEmpty() || result.isNull() || result.isUndefined()) {
                       done(false, {}, error.value(QStringLiteral("message")).toString(QObject::tr("This symbol cannot be renamed")));
                       return;
                   }
                   const QJsonObject o = result.toObject();
                   QString placeholder = o.value(QStringLiteral("placeholder")).toString();
                   if (placeholder.isEmpty()) {
                       // { range } or { defaultBehavior } or a bare Range: read the name out of the document.
                       const QJsonObject r = o.contains(QStringLiteral("range")) ? o.value(QStringLiteral("range")).toObject() : o;
                       const QJsonObject a = r.value(QStringLiteral("start")).toObject(), b = r.value(QStringLiteral("end")).toObject();
                       const QStringList lines = text.split(QLatin1Char('\n'));
                       const int l = a.value(QStringLiteral("line")).toInt();
                       if (l >= 0 && l < lines.size() && l == b.value(QStringLiteral("line")).toInt())
                           placeholder = lines.at(l).mid(a.value(QStringLiteral("character")).toInt(),
                                                         b.value(QStringLiteral("character")).toInt() - a.value(QStringLiteral("character")).toInt());
                   }
                   done(true, placeholder, {});
               });
}

void LspManager::rename(Document *doc, int line, int column, const QString &newName,
                        std::function<void(const QJsonObject &, const QString &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({}, tr("No language server is running for this file"));
        return;
    }
    QJsonObject params = positionParams(uri, line, column);
    params.insert(QStringLiteral("newName"), newName);
    c->request(QStringLiteral("textDocument/rename"), params, [done](const QJsonValue &result, const QJsonObject &error) {
        if (!error.isEmpty() || !result.isObject()) {
            done({}, error.value(QStringLiteral("message")).toString(QObject::tr("The server returned no changes")));
            return;
        }
        done(result.toObject(), {});
    });
}

bool LspManager::supportsReferences(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    if (!c || !c->isRunning())
        return false;
    const QJsonValue p = c->serverCapabilities().value(QStringLiteral("referencesProvider"));
    return !p.isUndefined() && !p.isNull() && p != false;
}

void LspManager::references(Document *doc, int line, int column, std::function<void(const QVector<LspLocation> &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    QJsonObject params = positionParams(uri, line, column);
    params.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("includeDeclaration"), true}});
    c->request(QStringLiteral("textDocument/references"), params, [done](const QJsonValue &result, const QJsonObject &error) {
        QVector<LspLocation> out;
        if (error.isEmpty())
            for (const QJsonValue &v : result.toArray())
                addLocation(out, v.toObject());
        done(out);
    });
}

// --- Semantic tokens, folding ranges, document symbols, call hierarchy ---------------------------------

namespace {
// Whether the capability is advertised (an options object or true).
bool provides(const QJsonObject &caps, const char *capability)
{
    const QJsonValue v = caps.value(QLatin1String(capability));
    return v.isObject() || v.toBool(false);
}

QString pathOfUri(const QString &uri)
{
    return JarSource::isJarUri(uri) ? JarSource::extract(uri) : QUrl(uri).toLocalFile();
}

QPair<int, int> startOf(const QJsonObject &range)
{
    const QJsonObject s = range.value(QStringLiteral("start")).toObject();
    return {s.value(QStringLiteral("line")).toInt(), s.value(QStringLiteral("character")).toInt()};
}

QPair<int, int> endOf(const QJsonObject &range)
{
    const QJsonObject e = range.value(QStringLiteral("end")).toObject();
    return {e.value(QStringLiteral("line")).toInt(), e.value(QStringLiteral("character")).toInt()};
}

LspCallItem parseCallItem(const QJsonObject &o, const QString &server)
{
    LspCallItem it;
    it.name = o.value(QStringLiteral("name")).toString();
    it.detail = o.value(QStringLiteral("detail")).toString();
    it.kind = o.value(QStringLiteral("kind")).toInt(12);
    it.server = server;
    it.path = pathOfUri(o.value(QStringLiteral("uri")).toString());
    const QPair<int, int> at = startOf(o.value(QStringLiteral("selectionRange")).toObject());
    it.line = at.first;
    it.column = at.second;
    it.raw = o;
    return it;
}

LspSymbol parseSymbol(const QJsonObject &o)
{
    LspSymbol sym;
    sym.name = o.value(QStringLiteral("name")).toString();
    sym.detail = o.value(QStringLiteral("detail")).toString();
    sym.kind = o.value(QStringLiteral("kind")).toInt();
    sym.deprecated = o.value(QStringLiteral("deprecated")).toBool();
    for (const QJsonValue &tag : o.value(QStringLiteral("tags")).toArray())
        if (tag.toInt() == 1)
            sym.deprecated = true;
    const QJsonObject range = o.value(QStringLiteral("range")).toObject();
    const QJsonObject selection = o.contains(QStringLiteral("selectionRange")) ? o.value(QStringLiteral("selectionRange")).toObject() : range;
    sym.line = startOf(range).first;
    sym.column = startOf(range).second;
    sym.endLine = endOf(range).first;
    sym.endColumn = endOf(range).second;
    sym.nameLine = startOf(selection).first;
    sym.nameColumn = startOf(selection).second;
    for (const QJsonValue &c : o.value(QStringLiteral("children")).toArray())
        sym.children << parseSymbol(c.toObject());
    return sym;
}
} // namespace

bool LspManager::supportsSemanticTokens(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    if (!c || !c->isRunning())
        return false;
    const QJsonObject p = c->serverCapabilities().value(QStringLiteral("semanticTokensProvider")).toObject();
    const QJsonValue full = p.value(QStringLiteral("full"));
    return !p.isEmpty() && (full.isObject() || full.toBool(false));
}

namespace {
// Five numbers per token: line delta, column delta (relative to the previous token when on the same line), length, type
// index, modifier bits.
void decodeSemanticTokens(const QVector<quint32> &data, LspSemanticTokens &out)
{
    int line = 0, column = 0;
    for (qsizetype i = 0; i + 4 < data.size(); i += 5) {
        const int dLine = int(data.at(i)), dStart = int(data.at(i + 1));
        if (dLine > 0) {
            line += dLine;
            column = dStart;
        } else {
            column += dStart;
        }
        LspSemanticToken t;
        t.column = column;
        t.length = int(data.at(i + 2));
        t.type = int(data.at(i + 3));
        t.modifiers = data.at(i + 4);
        if (t.length <= 0 || line > 5000000)
            continue;
        if (out.lines.size() <= line)
            out.lines.resize(line + 1);
        out.lines[line].append(t);
    }
    out.ok = true;
}
} // namespace

// Servers that offer deltas (semanticTokens/full/delta) are asked for the changes since their last answer, which is kept
// with the document: after an edit they send a few numbers instead of every token of the file again.
void LspManager::semanticTokens(Document *doc, std::function<void(const LspSemanticTokens &)> done)
{
    QString uri;
    LspClient *c = supportsSemanticTokens(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({});
        return;
    }
    const QJsonObject provider = c->serverCapabilities().value(QStringLiteral("semanticTokensProvider")).toObject();
    const QJsonObject legend = provider.value(QStringLiteral("legend")).toObject();
    LspSemanticTokens out;
    for (const QJsonValue &v : legend.value(QStringLiteral("tokenTypes")).toArray())
        out.types << v.toString();
    for (const QJsonValue &v : legend.value(QStringLiteral("tokenModifiers")).toArray())
        out.modifiers << v.toString();
    const QJsonValue full = provider.value(QStringLiteral("full"));
    const bool deltas = full.isObject() && full.toObject().value(QStringLiteral("delta")).toBool();
    const auto tracked = m_tracked.constFind(doc);
    const QString previous = deltas && tracked != m_tracked.constEnd() ? tracked->tokensResultId : QString();
    QJsonObject params{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}};
    if (!previous.isEmpty())
        params.insert(QStringLiteral("previousResultId"), previous);
    const QString method = previous.isEmpty() ? QStringLiteral("textDocument/semanticTokens/full") : QStringLiteral("textDocument/semanticTokens/full/delta");
    QPointer<Document> guard(doc);
    c->request(method, params, [this, guard, done, out, deltas, previous](const QJsonValue &result, const QJsonObject &error) mutable {
        auto it = guard ? m_tracked.find(guard) : m_tracked.end();
        Tracked *t = it != m_tracked.end() ? &it.value() : nullptr;
        auto forget = [t] {
            if (t) {
                t->tokensResultId.clear();
                t->tokensData.clear();
            }
        };
        if (!error.isEmpty() || !result.isObject()) {
            // Held back and replaced by a newer request before it was sent: the server's last answer still stands.
            if (error.value(QStringLiteral("message")).toString() != QLatin1String("superseded"))
                forget();
            done(out); // ok stays false: the editor keeps what it shows
            return;
        }
        const QJsonObject r = result.toObject();
        QVector<quint32> data;
        if (r.contains(QStringLiteral("edits"))) {
            // A delta applies to the answer it was asked against; another answer has replaced it meanwhile (two requests
            // in flight) or it does not fit: ask for everything again.
            if (!t || t->tokensResultId != previous || !LspSync::applyTokenEdits(t->tokensData, r.value(QStringLiteral("edits")).toArray())) {
                forget();
                if (guard)
                    semanticTokens(guard, done);
                else
                    done(out);
                return;
            }
            data = t->tokensData;
        } else {
            data = LspSync::tokenData(r.value(QStringLiteral("data")).toArray());
        }
        if (t && deltas) {
            t->tokensResultId = r.value(QStringLiteral("resultId")).toString();
            t->tokensData = t->tokensResultId.isEmpty() ? QVector<quint32>() : data;
        }
        decodeSemanticTokens(data, out);
        done(out);
    });
}

bool LspManager::supportsFoldingRanges(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    return c && c->isRunning() && provides(c->serverCapabilities(), "foldingRangeProvider");
}

void LspManager::foldingRanges(Document *doc, std::function<void(const QVector<LspFoldingRange> &, bool)> done)
{
    QString uri;
    LspClient *c = supportsFoldingRanges(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({}, false);
        return;
    }
    c->request(QStringLiteral("textDocument/foldingRange"), QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}},
               [done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspFoldingRange> out;
                   if (!error.isEmpty() || !result.isArray()) {
                       done(out, false);
                       return;
                   }
                   for (const QJsonValue &v : result.toArray()) {
                       const QJsonObject o = v.toObject();
                       LspFoldingRange r;
                       r.startLine = o.value(QStringLiteral("startLine")).toInt();
                       r.endLine = o.value(QStringLiteral("endLine")).toInt();
                       r.kind = o.value(QStringLiteral("kind")).toString();
                       if (r.endLine > r.startLine)
                           out << r;
                   }
                   done(out, true);
               });
}

bool LspManager::supportsDocumentSymbols(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    return c && c->isRunning() && provides(c->serverCapabilities(), "documentSymbolProvider");
}

void LspManager::documentSymbols(Document *doc, std::function<void(const QVector<LspSymbol> &, bool)> done)
{
    QString uri;
    LspClient *c = supportsDocumentSymbols(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({}, false);
        return;
    }
    c->request(QStringLiteral("textDocument/documentSymbol"), QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}},
               [done](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspSymbol> out;
                   if (!error.isEmpty() || !result.isArray()) {
                       done(out, false);
                       return;
                   }
                   for (const QJsonValue &v : result.toArray()) {
                       QJsonObject o = v.toObject();
                       if (o.contains(QStringLiteral("location"))) { // SymbolInformation: flat, the range sits in `location`
                           o.insert(QStringLiteral("range"), o.value(QStringLiteral("location")).toObject().value(QStringLiteral("range")));
                           o.insert(QStringLiteral("detail"), o.value(QStringLiteral("containerName")));
                       }
                       out << parseSymbol(o);
                   }
                   done(out, true);
               });
}

bool LspManager::supportsCallHierarchy(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    return c && c->isRunning() && provides(c->serverCapabilities(), "callHierarchyProvider");
}

void LspManager::prepareCallHierarchy(Document *doc, int line, int column, std::function<void(const QVector<LspCallItem> &)> done)
{
    QString uri;
    LspClient *c = supportsCallHierarchy(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({});
        return;
    }
    const QString server = m_tracked.value(doc).serverId;
    c->request(QStringLiteral("textDocument/prepareCallHierarchy"), positionParams(uri, line, column),
               [done, server](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspCallItem> out;
                   if (error.isEmpty())
                       for (const QJsonValue &v : result.toArray())
                           out << parseCallItem(v.toObject(), server);
                   done(out);
               });
}

void LspManager::callHierarchyCalls(const LspCallItem &item, bool incoming, std::function<void(const QVector<LspCallItem> &, bool)> done)
{
    LspClient *c = m_servers.value(item.server).client;
    if (!c || !c->isRunning()) {
        done({}, false);
        return;
    }
    const QString server = item.server;
    const QString parentPath = item.path;
    c->request(incoming ? QStringLiteral("callHierarchy/incomingCalls") : QStringLiteral("callHierarchy/outgoingCalls"),
               QJsonObject{{QStringLiteral("item"), item.raw}},
               [done, server, incoming, parentPath](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspCallItem> out;
                   if (!error.isEmpty()) {
                       done(out, false);
                       return;
                   }
                   for (const QJsonValue &v : result.toArray()) {
                       const QJsonObject o = v.toObject();
                       LspCallItem it = parseCallItem(o.value(incoming ? QStringLiteral("from") : QStringLiteral("to")).toObject(), server);
                       // fromRanges lie in the caller: the other function for incoming calls, the one we expanded for outgoing.
                       it.callSitePath = incoming ? it.path : parentPath;
                       for (const QJsonValue &r : o.value(QStringLiteral("fromRanges")).toArray())
                           it.callSites << startOf(r.toObject());
                       out << it;
                   }
                   done(out, true);
               });
}

bool LspManager::supportsTypeHierarchy(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    return c && c->isRunning() && provides(c->serverCapabilities(), "typeHierarchyProvider");
}

void LspManager::prepareTypeHierarchy(Document *doc, int line, int column, std::function<void(const QVector<LspCallItem> &)> done)
{
    QString uri;
    LspClient *c = supportsTypeHierarchy(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({});
        return;
    }
    const QString server = m_tracked.value(doc).serverId;
    c->request(QStringLiteral("textDocument/prepareTypeHierarchy"), positionParams(uri, line, column),
               [done, server](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspCallItem> out;
                   if (error.isEmpty())
                       for (const QJsonValue &v : result.toArray())
                           out << parseCallItem(v.toObject(), server);
                   done(out);
               });
}

void LspManager::typeHierarchy(const LspCallItem &item, bool supertypes, std::function<void(const QVector<LspCallItem> &, bool)> done)
{
    LspClient *c = m_servers.value(item.server).client;
    if (!c || !c->isRunning()) {
        done({}, false);
        return;
    }
    const QString server = item.server;
    c->request(supertypes ? QStringLiteral("typeHierarchy/supertypes") : QStringLiteral("typeHierarchy/subtypes"),
               QJsonObject{{QStringLiteral("item"), item.raw}}, [done, server](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspCallItem> out;
                   if (!error.isEmpty()) {
                       done(out, false);
                       return;
                   }
                   for (const QJsonValue &v : result.toArray())
                       out << parseCallItem(v.toObject(), server);
                   done(out, true);
               });
}

namespace {
LspCodeLens parseCodeLens(const QJsonObject &o, const QString &server)
{
    LspCodeLens lens;
    const QPair<int, int> at = startOf(o.value(QStringLiteral("range")).toObject());
    lens.line = at.first;
    lens.column = at.second;
    const QJsonObject cmd = o.value(QStringLiteral("command")).toObject();
    lens.title = cmd.value(QStringLiteral("title")).toString();
    lens.command = cmd.value(QStringLiteral("command")).toString();
    lens.arguments = cmd.value(QStringLiteral("arguments")).toArray();
    lens.raw = o;
    lens.server = server;
    return lens;
}
} // namespace

bool LspManager::supportsCodeLens(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    return c && c->isRunning() && provides(c->serverCapabilities(), "codeLensProvider");
}

void LspManager::codeLenses(Document *doc, std::function<void(const QVector<LspCodeLens> &, bool)> done)
{
    QString uri;
    LspClient *c = supportsCodeLens(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({}, false);
        return;
    }
    const QString server = m_tracked.value(doc).serverId;
    c->request(QStringLiteral("textDocument/codeLens"), QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}}},
               [done, server](const QJsonValue &result, const QJsonObject &error) {
                   QVector<LspCodeLens> out;
                   if (!error.isEmpty()) {
                       done(out, false);
                       return;
                   }
                   for (const QJsonValue &v : result.toArray())
                       out << parseCodeLens(v.toObject(), server);
                   done(out, true);
               });
}

void LspManager::resolveCodeLens(const LspCodeLens &lens, std::function<void(const LspCodeLens &)> done)
{
    LspClient *c = m_servers.value(lens.server).client;
    const QJsonObject caps = c && c->isRunning() ? c->serverCapabilities().value(QStringLiteral("codeLensProvider")).toObject() : QJsonObject();
    if (!caps.value(QStringLiteral("resolveProvider")).toBool()) {
        done(lens);
        return;
    }
    c->request(QStringLiteral("codeLens/resolve"), lens.raw, [done, lens](const QJsonValue &result, const QJsonObject &error) {
        if (!error.isEmpty() || !result.isObject()) {
            done(lens);
            return;
        }
        LspCodeLens out = parseCodeLens(result.toObject(), lens.server);
        out.line = lens.line; // the editor placed it already; a server may answer with the range it computed meanwhile
        out.column = lens.column;
        done(out);
    });
}

bool LspManager::hasCommand(const QString &serverId, const QString &command) const
{
    const LspClient *c = m_servers.value(serverId).client;
    if (!c || !c->isRunning() || command.isEmpty())
        return false;
    return c->serverCapabilities().value(QStringLiteral("executeCommandProvider")).toObject().value(QStringLiteral("commands")).toArray().contains(command);
}

void LspManager::executeCommand(const QString &serverId, const QString &command, const QJsonArray &arguments,
                                std::function<void(bool, const QString &)> done)
{
    LspClient *c = m_servers.value(serverId).client;
    if (!c || !c->isRunning()) {
        done(false, tr("The language server is not running"));
        return;
    }
    QJsonObject params{{QStringLiteral("command"), command}};
    if (!arguments.isEmpty())
        params.insert(QStringLiteral("arguments"), arguments);
    c->request(QStringLiteral("workspace/executeCommand"), params, [done](const QJsonValue &, const QJsonObject &error) {
        done(error.isEmpty(), error.value(QStringLiteral("message")).toString());
    });
}

QVector<LspLocation> LspManager::locationsOf(const QJsonValue &value)
{
    QVector<LspLocation> out;
    if (value.isArray()) {
        for (const QJsonValue &v : value.toArray())
            if (v.isObject())
                addLocation(out, v.toObject());
    } else if (value.isObject()) {
        addLocation(out, value.toObject());
    }
    return out;
}

bool LspManager::supportsWorkspaceSymbols(Document *doc) const
{
    const auto it = m_tracked.constFind(doc);
    if (it == m_tracked.constEnd() || !it->opened())
        return false;
    const LspClient *c = m_servers.value(it->serverId).client;
    return c && c->isRunning() && provides(c->serverCapabilities(), "workspaceSymbolProvider");
}

void LspManager::workspaceSymbols(Document *doc, const QString &query, std::function<void(const QVector<LspWorkspaceSymbol> &, bool)> done)
{
    QString uri;
    LspClient *c = supportsWorkspaceSymbols(doc) ? readyClientFor(doc, &uri) : nullptr;
    if (!c) {
        done({}, false);
        return;
    }
    c->request(QStringLiteral("workspace/symbol"), QJsonObject{{QStringLiteral("query"), query}}, [done](const QJsonValue &result, const QJsonObject &error) {
        QVector<LspWorkspaceSymbol> out;
        if (!error.isEmpty() || !result.isArray()) {
            done(out, false);
            return;
        }
        for (const QJsonValue &v : result.toArray()) {
            const QJsonObject o = v.toObject();
            const QJsonObject loc = o.value(QStringLiteral("location")).toObject();
            LspWorkspaceSymbol s;
            s.name = o.value(QStringLiteral("name")).toString();
            s.container = o.value(QStringLiteral("containerName")).toString();
            s.kind = o.value(QStringLiteral("kind")).toInt();
            s.path = pathOfUri(loc.value(QStringLiteral("uri")).toString());
            s.line = startOf(loc.value(QStringLiteral("range")).toObject()).first;
            s.column = startOf(loc.value(QStringLiteral("range")).toObject()).second;
            if (!s.name.isEmpty() && !s.path.isEmpty())
                out << s;
        }
        done(out, true);
    });
}
