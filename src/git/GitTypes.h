#pragma once

#include <QChar>
#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// Ordered by display priority: when a folder contains several kinds of change,
// the highest one wins (Ignored never propagates to parents).
enum class GitKind { None, Ignored, Untracked, Added, Renamed, Deleted, Modified, Conflicted };

enum class GitDiffMode {
    Unstaged, // index  -> working tree
    Staged,   // HEAD   -> index
    Head      // HEAD   -> working tree
};

// One entry of `git status`.
struct GitFileChange {
    QString path;        // absolute
    QString relPath;     // relative to the repository root
    QString origRelPath; // rename/copy source (relative), if any
    QChar staged = QLatin1Char('.');   // index column   (X)
    QChar unstaged = QLatin1Char('.'); // work tree column (Y)
    bool untracked = false;
    bool conflicted = false;

    bool isStaged() const { return !untracked && !conflicted && staged != QLatin1Char('.'); }
    bool isUnstaged() const { return !untracked && !conflicted && unstaged != QLatin1Char('.'); }

    GitKind kind() const
    {
        if (conflicted)
            return GitKind::Conflicted;
        if (untracked)
            return GitKind::Untracked;
        if (staged == QLatin1Char('D') || unstaged == QLatin1Char('D'))
            return GitKind::Deleted;
        if (staged == QLatin1Char('A'))
            return GitKind::Added;
        if (staged == QLatin1Char('R') || staged == QLatin1Char('C'))
            return GitKind::Renamed;
        return GitKind::Modified;
    }
};

// What the explorer needs to know about one path.
struct GitPathState {
    GitKind kind = GitKind::None;
    QChar staged = QLatin1Char('.');
    QChar unstaged = QLatin1Char('.');
    bool untracked = false;
    bool isFolder = false; // aggregated from descendants
    bool isStaged() const { return !untracked && staged != QLatin1Char('.') && staged != QLatin1Char('?'); }
    bool isUnstaged() const { return !untracked && unstaged != QLatin1Char('.'); }
};

struct GitBranchInfo {
    QString name; // "main" or "origin/main"
    bool current = false;
    bool remote = false;
    QString upstream;
};

// One line of `git blame`.
struct GitBlameLine {
    QString hash, author, summary;
    qint64 time = 0; // author time, seconds since the epoch
    bool committed() const { return !hash.isEmpty() && hash.count(QLatin1Char('0')) != hash.size(); }
};

struct GitCommitInfo {
    QString hash, shortHash, author, date, refs, subject;
};

// --- Presentation helpers shared by the explorer, the Source Control panel and the diff views ---

inline GitKind gitKindOfCode(QChar c)
{
    switch (c.toLatin1()) {
    case 'M': case 'T': return GitKind::Modified;
    case 'A': return GitKind::Added;
    case 'D': return GitKind::Deleted;
    case 'R': case 'C': return GitKind::Renamed;
    case '?': return GitKind::Untracked;
    case 'U': case '!': return GitKind::Conflicted;
    default: return GitKind::None;
    }
}

inline QString gitCodeName(QChar c)
{
    auto t = [](const char *s) { return QCoreApplication::translate("Git", s); };
    switch (c.toLatin1()) {
    case 'M': return t("Modified");
    case 'T': return t("Type changed");
    case 'A': return t("Added");
    case 'D': return t("Deleted");
    case 'R': return t("Renamed");
    case 'C': return t("Copied");
    case '?': return t("Untracked");
    case 'U': return t("Conflict");
    default: return {};
    }
}

// Letter shown in badges: the untracked marker is displayed as "U" like most editors do.
inline QChar gitBadgeLetter(QChar c)
{
    return c == QLatin1Char('?') ? QLatin1Char('U') : c;
}

inline QString gitDescribe(const GitPathState &s, bool conflicted = false)
{
    auto t = [](const char *x) { return QCoreApplication::translate("Git", x); };
    if (s.isFolder)
        return t("Contains changes");
    if (s.kind == GitKind::Ignored)
        return t("Ignored");
    if (conflicted || s.kind == GitKind::Conflicted)
        return t("Merge conflict");
    if (s.untracked)
        return t("Untracked (new file)");
    QStringList parts;
    if (s.isStaged())
        parts << t("Staged: %1").arg(gitCodeName(s.staged));
    if (s.isUnstaged())
        parts << t("Not staged: %1").arg(gitCodeName(s.unstaged));
    return parts.join(QStringLiteral("\n"));
}
