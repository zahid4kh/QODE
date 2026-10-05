#pragma once

#include "LspTypes.h"

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <functional>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

class Document;
class LspClient;
struct LspServerSpec;
class QTimer;

// Owns the language servers and keeps them in sync with the open documents. One server process per
// server kind (e.g. clangd) and project; servers start lazily when the first matching file opens.
class LspManager : public QObject
{
    Q_OBJECT
public:
    enum class Status { Idle, NotFound, Starting, Running, Crashed };

    struct ServerState {
        QString id;
        QString name; // display name
        Status status = Status::Idle;
        QString path;   // resolved executable
        QString detail; // "clangd 18.1.3", an error, ...
        int documents = 0; // open files this server handles
        bool syncing = false; // a build-file sync (Gradle / Maven re-import) is running
        QString progress;  // what the server reports it is busy with ("Importing Gradle project 40%"), or empty
        bool installable = false; // QODE can download it (LspInstaller)
        bool companion = false;   // ESLint / Tailwind: runs next to a file's main server
    };

    explicit LspManager(QObject *parent = nullptr);
    ~LspManager() override;

    // The project folder servers are rooted at; changing it stops every server.
    void setProjectRoot(const QString &root);
    void shutdown(); // stop every server (blocking, brief)

    void documentOpened(Document *doc);
    void documentSaved(Document *doc);
    void documentPathChanged(Document *doc);
    void buildFileSaved(const QString &path); // .pro / .pri saved; Gradle / Maven files re-sync the JVM servers
    // Tells the Java and Kotlin servers that the build files changed so they re-import the project (Gradle / Maven). With
    // `manual` the files are announced even when unchanged and a server that shows no sign of syncing is restarted.
    // Returns false when no JVM server is running (nothing to sync).
    bool syncBuildFiles(bool manual);
    bool hasBuildFiles() const;      // the project has a Gradle / Maven build file
    bool canSyncBuildFiles() const;  // ... and a Java / Kotlin server is running
    static bool isJvmBuildFile(const QString &path);

    QList<ServerState> servers() const;
    QString installHelp(const QString &serverId) const;
    QString serverPath(const QString &serverId) const; // configured override, "" = search PATH
    void setServerPath(const QString &serverId, const QString &path);
    void restart(const QString &serverId);
    QStringList logOf(const QString &serverId) const;
    bool hasCompileDatabase() const;
    // Flags worked out from the project's own build files (a qmake .pro); `source` gets the file name. Empty
    // when the project has a compile database or no recognised build file.
    QStringList detectedFlags(QString *source = nullptr) const;
    QString projectFlagsText() const;
    void setProjectFlagsText(const QString &text); // restarts the servers that use them
    // One flag per line, '#' comments skipped, {project} replaced.
    static QStringList parseFlags(const QString &text, const QString &projectRoot);

    // True when a running server handles this document, so hover/definition requests can be answered.
    bool isServed(Document *doc) const;
    // Markdown text of the symbol under (line, column), empty when there is none.
    void hover(Document *doc, int line, int column, std::function<void(const QString &)> done);
    // Where the symbol under (line, column) is defined; empty when unknown.
    void definition(Document *doc, int line, int column, std::function<void(const QVector<LspLocation> &)> done);

    // Colour literals of the document (CSS colours, Tailwind classes), merged from every server that provides them.
    void documentColors(Document *doc, std::function<void(const QVector<LspColor> &)> done);
    // The spellings the server offers for a colour picked by the user ("#ff0000", "rgb(255, 0, 0)", ...).
    void colorPresentations(Document *doc, const LspColor &color, const QColor &picked,
                            std::function<void(const QVector<LspColorPresentation> &)> done);
    // Rename: whether the file's server supports it; prepareRename gives the range / current name (or an error text);
    // rename returns the WorkspaceEdit to apply.
    bool supportsRename(Document *doc) const;
    void prepareRename(Document *doc, int line, int column,
                       std::function<void(bool ok, const QString &placeholder, const QString &error)> done);
    void rename(Document *doc, int line, int column, const QString &newName,
                std::function<void(const QJsonObject &edit, const QString &error)> done);
    bool supportsReferences(Document *doc) const;
    void references(Document *doc, int line, int column, std::function<void(const QVector<LspLocation> &)> done);

    // Completion candidates at (line, column). triggerKind: 1 invoked, 2 trigger character, 3 list was incomplete.
    // A newer request cancels the previous one (whose callback then never runs).
    void completion(Document *doc, int line, int column, int triggerKind, const QString &triggerChar,
                    std::function<void(const QVector<LspCompletionItem> &, bool incomplete)> done);

    // The per-file text edits of a WorkspaceEdit (`changes` or `documentChanges`); *ok is false when it holds something
    // that cannot be applied as text edits (file create / rename / delete, non-file URIs).
    static QHash<QString, QVector<LspTextEdit>> editsOf(const QJsonObject &edit, bool *ok);
    // 0-based lines of imports that organize-imports would remove (Kotlin); empty when unknown or none.
    void unusedImports(Document *doc, std::function<void(const QVector<int> &)> done);
    // True when the server of this document lists `command` among its executeCommandProvider commands.
    bool supportsCommand(Document *doc, const QString &command) const;
    // Accepts a completion item that carries a server command (Kotlin: adds the import, inserts the text). The server's
    // edits arrive through the apply-edit handler; done(false) means nothing happened and the caller should insert locally.
    void runCompletionCommand(Document *doc, const LspCompletionItem &item, int line, int column, std::function<void(bool)> done);
    // Asks the server to complete an item the list left unfinished (item.resolveData set: jdtls imports); `done` runs once.
    void resolveCompletion(Document *doc, const LspCompletionItem &item, std::function<void(const LspCompletionItem &)> done);
    // Quick fixes / refactorings for the range (Alt+Enter); diagnostics inside it are passed along.
    void codeActions(Document *doc, int startLine, int startColumn, int endLine, int endColumn,
                     std::function<void(const QVector<LspCodeAction> &)> done);
    // textDocument/formatting, waited for with a local event loop (save must stay synchronous). False when no running
    // server formats this document, it timed out or failed; *edits may be empty when the text is already formatted.
    bool formatting(Document *doc, int tabSize, bool spaces, int timeoutMs, QVector<LspTextEdit> *edits);
    void runCodeAction(Document *doc, const LspCodeAction &action, std::function<void(bool)> done);
    // Applies a WorkspaceEdit the server sends (workspace/applyEdit) or a code action carries; returns whether it went through.
    void setApplyEditHandler(std::function<bool(const QJsonObject &edit)> handler) { m_applyEdit = std::move(handler); }

    QVector<LspDiagnostic> diagnostics(const QString &path) const { return m_diagnostics.value(path); }
    int diagnosticCount(int severity) const;

signals:
    void statusChanged();
    void buildSyncFinished(const QString &serverName); // the re-import ended
    void diagnosticsChanged(const QString &path);

private:
    struct Server {
        const LspServerSpec *spec = nullptr;
        LspClient *client = nullptr;
        ServerState state;
        int restarts = 0;
        int syncGen = 0;          // invalidates the pending "did anything start?" check
        bool syncSawProgress = false;
        QHash<QString, QStringList> progress; // $/progress token -> {title, text shown} of the running operations
    };
    struct Tracked {
        QString uri, path, serverId; // serverId: the file's main server
        QStringList companions;      // ids of companion servers that also hold this document
        QSet<QString> openOn;        // servers that have received didOpen (cleared per server when it stops)
        int version = 0;
        bool dirty = false;
        bool opened() const { return openOn.contains(serverId); }
        QStringList servers() const { return QStringList{serverId} + companions; }
    };

    Server *ensureServer(const LspServerSpec &spec, const QString &filePath);
    void startServer(Server &s, const QString &rootPath);
    void stopServer(Server &s);
    void onServerReady(const QString &id);
    void onServerStopped(const QString &id, bool crashed);
    void progressChanged(Server &s);
    QStringList jvmBuildFiles() const;
    void onNotification(const QString &id, const QString &method, const QJsonValue &params);
    void track(Document *doc);
    void untrack(Document *doc, bool sendClose);
    void sendOpen(Document *doc, Tracked &t, const QString &serverId);
    // Every running server holding the document, main server first.
    QList<LspClient *> readyClientsFor(Document *doc, QString *uri);
    void flushChanges();
    void setStatus(Server &s, Status status, const QString &detail = {});
    QString rootFor(const QString &filePath) const;
    LspClient *readyClientFor(Document *doc, QString *uri);
    void clearDiagnosticsFor(const QString &path, const QString &serverId = {}); // empty id = every server
    void setDiagnosticsFor(const QString &path, const QJsonArray &list, const QString &serverId);
    void publishMerged(const QString &path);
    void pullDiagnostics(Document *doc); // servers with diagnosticProvider (Kotlin) are asked instead of pushing

    QString m_root;
    QHash<QString, Server> m_servers;
    QHash<Document *, Tracked> m_tracked;
    QHash<QString, QVector<LspDiagnostic>> m_diagnostics;                    // merged over all servers
    QHash<QString, QHash<QString, QVector<LspDiagnostic>>> m_diagBySource;  // path -> server id -> its diagnostics
    QTimer *m_changeTimer;
    bool handleApplyEdit(const QJsonObject &edit);
    std::function<bool(const QJsonObject &)> m_applyEdit;
    bool m_capturing = false; // unusedImports() is running: the next workspace/applyEdit is recorded, not applied
    QJsonObject m_captured;
    QVector<QPair<QPointer<LspClient>, int>> m_completionRequests; // in flight, so a newer request can cancel them
};
