#pragma once

#include "GitTypes.h"

#include <QHash>
#include <QObject>
#include <QSet>

#include <functional>

class QFileSystemWatcher;
class QTimer;

// Thin asynchronous wrapper around the `git` command line for the open project.
// It caches `git status` and derives everything the UI needs from it; all git
// processes run in the background so the UI never blocks.
class GitRepository : public QObject
{
    Q_OBJECT
public:
    struct Result {
        int exitCode = -1;
        QByteArray out;
        QString err;
        bool ok() const { return exitCode == 0; }
    };
    using Callback = std::function<void(const Result &)>;

    explicit GitRepository(QObject *parent = nullptr);

    bool gitAvailable() const { return !m_gitExe.isEmpty(); }
    // The directory of the open project ("" => none). Detects the enclosing repository.
    void setWorkDirectory(const QString &dir);
    QString workDirectory() const { return m_workDir; }
    bool isRepo() const { return m_isRepo; }
    bool isResolved() const { return m_resolved; } // repository detection finished
    QString root() const { return m_root; }
    bool busy() const { return m_busy > 0; }

    // --- Cached status ---------------------------------------------------------
    QString branch() const { return m_branch; }
    bool isDetached() const { return m_detached; }
    QString upstream() const { return m_upstream; }
    int ahead() const { return m_ahead; }
    int behind() const { return m_behind; }
    bool hasCommits() const { return m_hasCommits; }
    QString headOid() const { return m_headOid; }
    const QList<GitFileChange> &changes() const { return m_changes; }
    QList<GitFileChange> stagedChanges() const;
    QList<GitFileChange> unstagedChanges() const; // work tree changes and untracked files
    QList<GitFileChange> conflicts() const;
    const GitFileChange *changeFor(const QString &absPath) const;
    const QList<GitBranchInfo> &branches() const { return m_branches; }

    GitPathState stateOf(const QString &absPath) const; // works for files and folders
    bool isIgnored(const QString &absPath) const;
    QString relativePath(const QString &absPath) const; // "" when outside the repository
    QString absolutePath(const QString &relPath) const;

    // --- Refreshing ------------------------------------------------------------
    void refresh();          // now (coalesced with a running refresh)
    void scheduleRefresh();  // debounced

    // --- Operations (asynchronous; failures are reported through errorOccurred) --
    void init();
    void stage(const QStringList &absPaths);
    void unstage(const QStringList &absPaths);
    void stageAll(QObject *ctx = nullptr, std::function<void(bool)> done = {});
    void unstageAll();
    void discard(const QStringList &absPaths); // reverts work tree changes / deletes untracked files
    void commit(const QString &message, bool amend, QObject *ctx, std::function<void(bool)> done);
    void checkout(const QString &branch);
    void createBranch(const QString &name);
    void deleteBranch(const QString &name, bool force);
    void fetch();
    void pull();
    void push();
    void stash(bool includeUntracked);
    void stashPop();
    void addToGitignore(const QString &absPath);

    // --- Queries ---------------------------------------------------------------
    // `git cat-file blob <spec>` e.g. "HEAD:src/a.cpp" or ":src/a.cpp" (index).
    void readBlob(const QString &spec, QObject *ctx, Callback cb);
    void log(int limit, QObject *ctx, std::function<void(const QList<GitCommitInfo> &)> cb);
    void commitPatch(const QString &hash, QObject *ctx, std::function<void(const QString &)> cb);
    void lastCommitMessage(QObject *ctx, std::function<void(const QString &)> cb);

signals:
    void repositoryChanged(); // repo detected / lost / switched
    void statusChanged();     // fresh status available
    void busyChanged(bool busy);
    void errorOccurred(const QString &title, const QString &detail);
    void operationFinished(const QString &title, const QString &output);

private:
    void run(const QStringList &args, QObject *ctx, Callback cb, const QByteArray &input = {}, int timeoutMs = 0);
    void runOp(const QString &title, const QList<QStringList> &steps, const QByteArray &input = {}, int timeoutMs = 0,
               std::function<void(bool)> done = {});
    void runStep(const QString &title, const QList<QStringList> &steps, int index, const QByteArray &input, int timeoutMs,
                 std::function<void(bool)> done);
    void finishOp(const QString &title, bool ok, const QString &text, const std::function<void(bool)> &done);
    void setBusy(int delta);

    void discover();
    void markNotRepo();
    void parseStatus(const QByteArray &out);
    void loadIgnored();
    void loadBranches();
    void finishRefresh();
    void watchGitDir();
    void clearWatches();
    void onGitPathChanged(const QString &path);
    QStringList relPaths(const QStringList &absPaths, bool includeRenameSources) const;

    QString m_gitExe;
    QString m_workDir, m_root, m_gitDir;
    bool m_isRepo = false;
    bool m_resolved = false;
    bool m_refreshing = false;
    bool m_refreshAgain = false;
    int m_busy = 0;
    int m_generation = 0; // bumped when the project changes so stale replies are ignored
    QTimer *m_timer;
    QFileSystemWatcher *m_watcher;

    QString m_branch, m_upstream, m_headOid;
    bool m_detached = false, m_hasCommits = false;
    int m_ahead = 0, m_behind = 0;
    QList<GitFileChange> m_changes;
    QHash<QString, int> m_changeIndex;        // abs path -> index into m_changes
    QHash<QString, GitKind> m_folderKinds;    // abs dir path -> strongest kind below it
    QSet<QString> m_ignored;                  // abs paths (files or whole directories)
    QList<GitBranchInfo> m_branches;
};
