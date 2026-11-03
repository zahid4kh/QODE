#pragma once

#include "ai/AiChat.h"
#include "agent/AgentJournal.h"
#include "ai/AiEdits.h"
#include "agent/CodeIndex.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QProcess;

// The local tools the agent hands to the model (Gemini function calling, Claude tool use). Every path is confined to the project root;
// edits go through the editor tabs (undoable, unsaved), the rest touches the disk.
namespace agent {

enum class Risk {
    ReadOnly,   // runs at once
    Edit,       // changes project files through the editor; runs at once (undo + git protect the user)
    Dangerous   // deleting, running commands: the user is asked first
};

struct ToolResult {
    QJsonObject response;  // goes back to the model as the functionResponse
    bool ok = true;
    QString summary;       // one line for the transcript
    QString output;        // the full result, shown when the row is folded out (Claude Code only)
    QString call;          // the call in words ("Read src/a.cpp 1-200")
    bool hasChange = false;
    AiChange change;       // edit tools: the diff for the transcript
    QJsonArray images;     // [{mimeType, data (base64)}]: pictures the model gets with the response (MCP tools)
};

// A tool from outside QODE (an MCP server's): declared after the built-in tools, carried out by `run`.
struct ExtraTool {
    QString name, description;
    QJsonObject schema;    // JSON Schema of the arguments (Gemini gets it as parametersJsonSchema)
    bool ask = true;       // the user approves each call ("Allow for this chat" covers the tool)
    QString label;         // the call in words: "blender › get_scene_info"
    std::function<ToolResult(const QJsonObject &args)> run;
};

// One language-server diagnostic.
struct Diag {
    QString path;           // absolute
    int line = 0, column = 0; // 1-based
    int severity = 1;       // 1 error, 2 warning, 3 information, 4 hint
    QString message, source, code;
};

class AgentTools
{
public:
    struct Hooks {
        // Text of a file (the open buffer when there is one); false when it cannot be read or is not text.
        std::function<bool(const QString &absolute, QString *text)> readFile;
        // Applies edits through the editor tabs and reports one outcome per edit.
        std::function<QVector<AiEdits::Outcome>(const QVector<AiEdits::Edit> &)> apply;
        // The project index (absolute paths); may be empty, then the tools walk the folder.
        std::function<QStringList()> projectFiles;
        // Open tabs: absolute path + modified flag.
        std::function<QVector<QPair<QString, bool>>()> openFiles;
        // Writes the given open documents to disk (commands and moves must see the edits).
        std::function<void(const QStringList &absolute)> saveFiles;
        std::function<void(const QString &absolute)> pathDeleted;
        std::function<void(const QString &from, const QString &to)> pathMoved;
        // Git changed the working tree or index: refresh the Version Control panel.
        std::function<void()> changed;
        // Diagnostics of one file, or of every open file when `abs` is empty, once the server has reported on the current
        // text (`afterEdit`: the check after an edit, which may wait for a server that is still starting). `settle`: then
        // also wait until the servers' reports have been quiet for a moment, because a file's report can still change
        // when another file edited at the same time is processed. Returns a note for the model when no server serves
        // the file or it did not answer in time.
        std::function<QString(const QString &abs, QVector<Diag> *rows, bool afterEdit, bool settle)> diagnostics;
        // What the server reported last, without waiting (the errors a file had before an edit); false when it has not
        // reported on the file yet.
        std::function<bool(const QString &abs, QVector<Diag> *rows)> knownDiagnostics;
        // A language server checks this file (running or starting): its errors are worth fetching after an edit.
        std::function<bool(const QString &abs)> checksFile;
        // Asks the user. `tool` is the function name; false = denied.
        std::function<bool(const QString &tool, const QString &title, const QString &detail)> approve;
        // Drives the window around the chat (panels, theme, editor options, language servers; MainWindowIde.cpp). Paths in
        // `args` are absolute and already confined to the project. An "error" key in the answer means it failed.
        std::function<QJsonObject(const QString &action, const QJsonObject &args)> ide;
        // Ranked sections of the codebase index for semantic_search (CodeIndex::search; `scope` project-relative, "" = all).
        std::function<QVector<CodeIndex::Hit>(const QString &query, const QString &scope, int max, QString *note)> codeSearch;
    };

    void setRoot(const QString &root);
    // The journal the changes are recorded in, and the number of the current turn (see AgentSession).
    void setJournal(Journal *journal) { m_journal = journal; }
    void setTurn(int turn) { m_turn = turn; }
    bool cancelled() const { return m_cancelled; }
    void setHooks(const Hooks &hooks) { m_hooks = hooks; }
    // Set before each run (the declarations stay the same within it).
    void setExtraTools(const QVector<ExtraTool> &tools) { m_extra = tools; }
    // Tool groups beyond the core ones that every project gets ("preview", "servers", "http", "jvm_debug"): only the
    // groups set here are declared and callable. Set before each run; a chat's groups only grow (see AgentPanel).
    void setGroups(const QStringList &groups);
    // The groups in their fixed order (also the order of their prompt sections).
    static const QStringList &allGroups();
    // What the system prompt says about the set groups (empty when none is set).
    QString groupPrompt() const;
    const QString &root() const { return m_root; }

    // Declarations for the generateContent request: [{functionDeclarations: [...]}].
    QJsonArray toolsJson() const;
    // The same tools for the Claude Messages API: [{name, description, input_schema, eager_input_streaming}].
    QJsonArray claudeToolsJson() const;
    // Runs one call (asking the user first when the tool needs it).
    ToolResult execute(const QString &name, const QJsonObject &args);
    // The tool's name for the status line: an MCP tool's label ("blender › look"), else the name itself.
    QString label(const QString &name) const;
    // The call in words, for the transcript ("Read src/a.cpp:1-200").
    QString describe(const QString &name, const QJsonObject &args) const;
    // The call asked to end the turn (`done` on a tool that offers it): no request follows when every call of the turn did.
    bool endsTurn(const QString &name, const QJsonObject &args) const;
    // Stops a running command; the current and later calls of this run return "cancelled".
    void cancel();
    void resetCancel() { m_cancelled = false; }

    // Files the agent changed since the last flush (kept for saving before commands).
    QStringList touchedFiles() const { return m_touched.values(); }

    // Diagnostics after edits: once a round of calls has run, the files its edits changed that a language server checks
    // (project-relative, in order), then the check of each. checkEdited answers with the file's errors in full and its
    // warnings counted; errors the file already had before the round are marked. AgentRun puts the answer into the
    // round's last edit of the file, so the model sees what it broke before it replies.
    QStringList filesToCheck(const QVector<ToolResult> &results);
    ToolResult checkEdited(const QString &relativePath, bool settle);
    static QString checkCall(const QString &relativePath) { return QStringLiteral("Check %1 for errors").arg(relativePath); }

private:
    struct Spec {
        QString name, description;
        QJsonObject parameters;
        Risk risk = Risk::ReadOnly;
        std::function<ToolResult(const QJsonObject &)> run;
        std::function<QString(const QJsonObject &)> call; // the call in words
        std::function<QString(const QJsonObject &)> detail; // what the approval dialog shows
        QString group = QStringLiteral("core"); // core tools are always declared
    };
    bool active(const Spec &s) const { return s.group == QLatin1String("core") || m_groups.contains(s.group); }
    const Spec *find(const QString &name) const; // an active spec

    void registerAll();
    void registerServerTools(); // AgentToolsServers.cpp
    void registerPreviewTools(); // AgentToolsPreview.cpp
    void registerHttpTools();    // AgentToolsHttp.cpp
    void registerDebugTools();   // AgentToolsDebug.cpp
    ToolResult executeExtra(const ExtraTool &tool, const QJsonObject &args);
    static QString extraCall(const ExtraTool &tool, const QJsonObject &args);
    void add(Spec spec) { m_specs.append(std::move(spec)); }

    // paths
    bool resolve(const QString &input, QString *absolute, QString *error, bool forWrite = false) const;
    QString relative(const QString &absolute) const;
    QStringList indexedFiles() const;
    bool readText(const QString &absolute, QString *text, QString *error) const;
    void flushEdits();

    // tool bodies
    ToolResult listDirectory(const QJsonObject &a);
    ToolResult directoryTree(const QJsonObject &a);
    ToolResult findFiles(const QJsonObject &a);
    ToolResult searchText(const QJsonObject &a);
    ToolResult semanticSearch(const QJsonObject &a);
    ToolResult readFileTool(const QJsonObject &a);
    ToolResult findTextRange(const QJsonObject &a);
    ToolResult openFilesTool(const QJsonObject &a);
    ToolResult gitTool(const QString &what, const QJsonObject &a);
    ToolResult editRange(const QJsonObject &a);
    ToolResult insertLines(const QJsonObject &a);
    ToolResult replaceText(const QJsonObject &a);
    ToolResult createFile(const QJsonObject &a);
    ToolResult createDirectory(const QJsonObject &a);
    ToolResult movePath(const QJsonObject &a, bool copy);
    ToolResult deletePath(const QJsonObject &a);
    ToolResult runCommand(const QJsonObject &a);
    ToolResult listChanges(const QJsonObject &a);
    ToolResult undoChanges(const QJsonObject &a);
    ToolResult redoChanges(const QJsonObject &a);
    ToolResult diagnosticsTool(const QJsonObject &a);
    ToolResult terminalOutputTool(const QJsonObject &a);
    QString diagLine(const Diag &d) const;
    // The IDE tools: everything goes through Hooks::ide.
    ToolResult ideTool(const QString &action, const QJsonObject &args, const QString &summary);
    ToolResult openFileTool(const QJsonObject &a);
    ToolResult editorCommand(const QJsonObject &a);
    // The server tools (AgentToolsServers.cpp).
    ToolResult serverStatusTool(const QJsonObject &a);
    ToolResult serverControlTool(const QString &what, const QJsonObject &a); // start, restart, stop
    ToolResult serverLogTool(const QJsonObject &a);
    // The preview tools (AgentToolsPreview.cpp): one IDE action each; a screenshot's picture goes into `images`.
    ToolResult previewTool(const QString &action, const QJsonObject &a);
    ToolResult httpRequestTool(const QJsonObject &a); // AgentToolsHttp.cpp
    // The debugger tools (AgentToolsDebug.cpp).
    ToolResult debugTool(const QString &action, const QJsonObject &a);
    ToolResult debugWait(int stopsBefore, int seconds, const QString &what);

    struct Before {
        bool existed = false;
        QString text;
    };
    // The file as it is now (raw, for the journal); false when it exists but cannot be captured.
    bool snapshot(const QString &abs, Before *b) const;
    void record(const QString &abs, const Before &b);

    // Runs one Edit through the hook and shapes the answer.
    ToolResult applyOne(const AiEdits::Edit &edit, const QString &call);

    QString m_root;       // canonical
    Hooks m_hooks;
    QVector<Spec> m_specs;
    QVector<ExtraTool> m_extra;
    QSet<QString> m_groups;
    QHash<QString, QString> m_groupText; // a group's section of the system prompt
    QSet<QString> m_touched;   // absolute paths edited by the agent
    QSet<QString> m_created;   // files the agent created (the project index may not know them yet)
    QSet<QString> m_gone;      // files the agent deleted or moved away
    struct ErrorsBefore {
        bool known = false;  // the server had reported on the file
        QStringList errors;  // source + message
    };
    QHash<QString, ErrorsBefore> m_errorsBefore; // edited, not yet checked: the errors the file had before
    Journal *m_journal = nullptr;
    int m_turn = 0;
    QProcess *m_process = nullptr;
    bool m_cancelled = false;
};

} // namespace agent
