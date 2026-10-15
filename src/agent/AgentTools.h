#pragma once

#include "ai/AiChat.h"
#include "agent/AgentJournal.h"
#include "ai/AiEdits.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QProcess;

// The local tools the agent tab hands to Gemini through function calling. Every path is confined to the project root;
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
    QString call;          // the call in words ("Read src/a.cpp 1-200")
    bool hasChange = false;
    AiChange change;       // edit tools: the diff for the transcript
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
        // Diagnostics of one file (waits until the server has had time to analyse it), or of every open file when `abs` is
        // empty. Returns a note for the model when there is nothing to report because no server serves the file.
        std::function<QString(const QString &abs, QVector<Diag> *rows)> diagnostics;
        // Asks the user. `tool` is the function name; false = denied.
        std::function<bool(const QString &tool, const QString &title, const QString &detail)> approve;
    };

    void setRoot(const QString &root);
    // The journal the changes are recorded in, and the number of the current turn (see AgentSession).
    void setJournal(Journal *journal) { m_journal = journal; }
    void setTurn(int turn) { m_turn = turn; }
    bool cancelled() const { return m_cancelled; }
    void setHooks(const Hooks &hooks) { m_hooks = hooks; }
    const QString &root() const { return m_root; }

    // Declarations for the generateContent request: [{functionDeclarations: [...]}].
    QJsonArray toolsJson() const;
    // Runs one call (asking the user first when the tool needs it).
    ToolResult execute(const QString &name, const QJsonObject &args);
    // The call in words, for the transcript ("Read src/a.cpp:1-200").
    QString describe(const QString &name, const QJsonObject &args) const;
    // Stops a running command; the current and later calls of this run return "cancelled".
    void cancel();
    void resetCancel() { m_cancelled = false; }

    // Files the agent changed since the last flush (kept for saving before commands).
    QStringList touchedFiles() const { return m_touched.values(); }

private:
    struct Spec {
        QString name, description;
        QJsonObject parameters;
        Risk risk = Risk::ReadOnly;
        std::function<ToolResult(const QJsonObject &)> run;
        std::function<QString(const QJsonObject &)> call; // the call in words
        std::function<QString(const QJsonObject &)> detail; // what the approval dialog shows
    };

    void registerAll();
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
    QSet<QString> m_touched;   // absolute paths edited by the agent
    QSet<QString> m_created;   // files the agent created (the project index may not know them yet)
    QSet<QString> m_gone;      // files the agent deleted or moved away
    Journal *m_journal = nullptr;
    int m_turn = 0;
    QProcess *m_process = nullptr;
    bool m_cancelled = false;
};

} // namespace agent
