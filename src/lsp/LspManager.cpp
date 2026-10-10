#include "LspManager.h"

#include "JarSource.h"
#include "LspClient.h"
#include "LspServers.h"
#include "editor/Document.h"
#include "project/PythonEnv.h"
#include "project/QmakeProject.h"
#include "settings/SettingsManager.h"

#include <QCryptographicHash>
#include <QDir>
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
#include <QTimer>
#include <QUrl>

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
                         {QStringLiteral("configuration"), QJsonObject{{QStringLiteral("updateBuildConfiguration"), QStringLiteral("automatic")}}}}}};
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
        // typescript-language-server does not look next to itself for TypeScript: use the project's own version when it
        // has one (so the editor agrees with its build), otherwise the copy installed together with the server.
        const QString rel = QStringLiteral("node_modules/typescript/lib/tsserver.js");
        for (const QString &dir : {rootPath, LspServers::managedDir(s.spec->managedId)})
            if (QFileInfo::exists(dir + QLatin1Char('/') + rel)) {
                options.insert(QStringLiteral("tsserver"), QJsonObject{{QStringLiteral("path"), dir + QLatin1Char('/') + rel}});
                break;
            }
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
        const QString workspace = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE/lsp/java/") + key;
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
    s.state.progress.clear();
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
        s.client->deleteLater();
        s.client = nullptr;
    }
    s.progress.clear();
    s.state.progress.clear();
    for (Tracked &t : m_tracked)
        t.openOn.remove(id);
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
                        QStringLiteral("/QODE/lsp/clangd-db/") + key;
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
    if (!spec)
        return;
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
    t.version = qMax(t.version, 1);
    t.openOn.insert(serverId);
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
        if (!t.dirty || t.openOn.isEmpty())
            continue;
        t.dirty = false;
        ++t.version;
        const QString text = it.key()->text();
        for (const QString &id : t.servers()) {
            Server &s = m_servers[id];
            if (!t.openOn.contains(id) || !s.client || !s.client->isRunning())
                continue;
            s.client->notify(QStringLiteral("textDocument/didChange"),
                             QJsonObject{{QStringLiteral("textDocument"),
                                          QJsonObject{{QStringLiteral("uri"), t.uri}, {QStringLiteral("version"), t.version}}},
                                         {QStringLiteral("contentChanges"), QJsonArray{QJsonObject{{QStringLiteral("text"), text}}}}});
        }
        pullDiagnostics(it.key());
    }
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
        // jdtls adds an item's import (additionalTextEdits) only when the item is resolved.
        bool resolvable = false;
        for (auto it = m_servers.constBegin(); it != m_servers.constEnd(); ++it)
            if (it->client == c && it->spec && it->spec->resolvesCompletions)
                resolvable = c->serverCapabilities().value(QStringLiteral("completionProvider")).toObject().value(QStringLiteral("resolveProvider")).toBool();
        const int id = c->request(QStringLiteral("textDocument/completion"), params,
                                  [gather, resolvable](const QJsonValue &result, const QJsonObject &error) {
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
                                              if (resolvable)
                                                  gather->items.last().resolveData = v.toObject();
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

// completionItem/resolve fills in what the server left out of the list (jdtls: the import line of a type that is not
// imported yet). `done` always runs once: with the resolved item, or the original after a timeout / on error.
void LspManager::resolveCompletion(Document *doc, const LspCompletionItem &item, std::function<void(const LspCompletionItem &)> done)
{
    QString uri;
    LspClient *c = item.resolveData.isEmpty() ? nullptr : readyClientFor(doc, &uri);
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
        }
        finish(out);
    });
}

void LspManager::codeActions(Document *doc, int startLine, int startColumn, int endLine, int endColumn,
                             std::function<void(const QVector<LspCodeAction> &)> done)
{
    QString uri;
    const QList<LspClient *> clients = readyClientsFor(doc, &uri);
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
        c->request(QStringLiteral("textDocument/codeAction"),
                   QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}}},
                               {QStringLiteral("range"), range},
                               {QStringLiteral("context"), QJsonObject{{QStringLiteral("diagnostics"), diagnostics}}}},
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

void LspManager::definition(Document *doc, int line, int column, std::function<void(const QVector<LspLocation> &)> done)
{
    QString uri;
    LspClient *c = readyClientFor(doc, &uri);
    if (!c) {
        done({});
        return;
    }
    QPointer<LspClient> guard(c);
    c->request(QStringLiteral("textDocument/definition"), positionParams(uri, line, column),
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
        progressChanged(s);
        emit statusChanged();
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
            QString text;
            if (task.contains(QStringLiteral("Gradle")) || task.contains(QStringLiteral("Maven")) || task.contains(QStringLiteral("mport"))
                || task.contains(QStringLiteral("ynchroni")))
                text = task.contains(QStringLiteral("Maven")) ? tr("Syncing Maven project") : tr("Syncing Gradle project");
            else if (task.startsWith(QLatin1String("Search")))
                text = tr("Indexing");
            else
                text = QString(task).remove(QStringLiteral("...")).trimmed();
            const int total = o.value(QStringLiteral("totalWork")).toInt();
            if (total > 0 && text != tr("Indexing"))
                text += QStringLiteral(" %1%").arg(qBound(0, o.value(QStringLiteral("workDone")).toInt() * 100 / total, 100));
            s.progress.insert(token, {task, text});
        }
        s.state.progress = s.progress.isEmpty() ? QString() : s.progress.constBegin()->value(1);
        progressChanged(s);
        emit statusChanged();
        return;
    }
    if (method == QLatin1String("workspace/diagnostic/refresh")) {
        for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it)
            if (it->servers().contains(id) && it->openOn.contains(id))
                pullDiagnostics(it.key());
        return;
    }
    if (method != QLatin1String("textDocument/publishDiagnostics"))
        return;
    const QJsonObject o = params.toObject();
    setDiagnosticsFor(QUrl(o.value(QStringLiteral("uri")).toString()).toLocalFile(), o.value(QStringLiteral("diagnostics")).toArray(), id);
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
        QPointer<LspClient> guard(client);
        client->request(QStringLiteral("textDocument/diagnostic"),
                        QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), it->uri}}}},
                        [this, path, guard, id](const QJsonValue &result, const QJsonObject &error) {
                            if (!guard || !error.isEmpty())
                                return;
                            const QJsonObject r = result.toObject();
                            if (r.value(QStringLiteral("kind")).toString() == QLatin1String("full"))
                                setDiagnosticsFor(path, r.value(QStringLiteral("items")).toArray(), id);
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
