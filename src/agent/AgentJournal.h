#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

// What the agent did to the project, kept so it can be undone and redone: by turn, by file, or by the lines of a file.
// Qt Core only. The files themselves are reached through FileAccess, so the editor tabs stay in sync.
namespace agent {

bool copyTree(const QString &from, const QString &to); // file or folder

struct FileAccess {
    std::function<bool(const QString &abs, QString *text)> read;          // open buffer or disk; false = does not exist
    std::function<bool(const QString &abs, const QString &text)> write;   // create or replace, and save to disk
    std::function<bool(const QString &abs)> remove;                       // delete a file the agent created
    std::function<bool(const QString &abs, QString *trashPath)> trash;    // delete into the trash (recoverable)
    std::function<bool(const QString &from, const QString &to)> rename;   // files and folders; open tabs follow
};

// One reversible step the agent took.
struct Op {
    enum Kind { Text, Move, Trash, Copy };
    Kind kind = Text;
    int seq = 0;       // chronological order, never reused
    int turn = 0;      // the user message it belongs to
    QString path;      // project-relative: the file (Text, Trash, Copy: the copy) or the destination of a Move
    QString from;      // Move / Copy: the source
    bool existedBefore = false, existsAfter = false; // Text
    QString before, after;                           // Text: whole file
    QString trash;     // Trash: where the file went; Copy: where the copy went while undone

    QJsonObject toJson() const;
    static Op fromJson(const QJsonObject &o);
};

class Journal
{
public:
    enum { Last = -1, All = -2 };

    struct Scope {
        int turn = Last;       // a turn, Last (the latest turn that changed anything) or All
        QString path;          // only this file (project-relative); empty = every file
        int startLine = 0, endLine = 0; // with a path: only the changes touching these lines (1-based, in the file as it is now)
        bool force = false;    // overwrite files that were changed after the agent left them
    };
    struct Result {
        QStringList done;       // what was reverted / re-applied
        QStringList conflicts;  // what was not, and why
        QStringList conflictPaths; // the files of those conflicts
        QString error;          // nothing to do / unsupported
    };
    struct Entry {
        int turn = 0;
        QString path;
        QString what;           // "edited", "created", "deleted", "moved from x", "copied from x"
        int added = 0, removed = 0;
    };

    void setRoot(const QString &root) { m_root = root; }
    void setAccess(const FileAccess &access) { m_io = access; }
    void clear();
    QJsonObject toJson() const;
    void load(const QJsonObject &o);

    // Recording (called by the tools after they changed something). A new change drops the redo history.
    void recordText(int turn, const QString &rel, bool existedBefore, const QString &before, bool existsAfter, const QString &after);
    void recordMove(int turn, const QString &from, const QString &to);
    void recordTrash(int turn, const QString &rel, const QString &trashPath);
    void recordCopy(int turn, const QString &from, const QString &to);

    bool isEmpty() const { return m_ops.isEmpty(); }
    int lastTurn() const;                               // -1 when nothing is recorded
    QVector<Entry> entries(int turn) const;             // turn: a number, Last or All
    bool canRedo(int turn = All) const;                 // All = anything at all
    QVector<Entry> redoEntries() const;                 // what the next redo would bring back

    Result undo(const Scope &scope);
    Result redo(const QString &path = QString(), bool force = false);

private:
    struct RedoItem {
        std::shared_ptr<Op> op;
        bool viaRevert = false;            // a line-level undo: redoing means reverting that op again
        std::shared_ptr<Op> patched;       // the journal op whose `after` the line-level undo changed
        QString patchedOldAfter;
    };
    struct RedoGroup {
        QVector<RedoItem> items;
    };

    QString abs(const QString &rel) const { return m_root + QLatin1Char('/') + rel; }
    bool touches(const Op &op, const QString &rel) const { return op.path == rel || op.from == rel; }
    bool revertOp(Op &op, bool force, QString *why);
    bool applyOp(Op &op, bool force, QString *why);
    bool textIs(const QString &rel, bool exists, const QString &text) const;
    Result undoLines(const Scope &scope, int turn);
    void add(const Op &op);
    static QString label(const Op &op);

    QString m_root;
    FileAccess m_io;
    QVector<std::shared_ptr<Op>> m_ops;   // chronological
    QVector<RedoGroup> m_redo;
    int m_nextSeq = 1;
};

} // namespace agent
