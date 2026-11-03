#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Local history: timestamped copies of files, kept outside the project in ~/.local/share/QODE-AGENTIC/history, so a
// version can be compared and restored without git. A copy is taken on every save (plus the version on disk before it,
// when no copy has it yet), before QODE edits or a refactor rewrites a file, and before a file is deleted.
//
// One folder per file (a hash of its path) holds `path` (the file's absolute path) and the versions, zlib-compressed,
// named `<msecs>-<kind>-<content hash>.z`. Identical consecutive versions are stored once, and auto save keeps one version
// a minute (a newer auto-saved version replaces the one from the same minute). Files over 2 MB and binary files are
// skipped; a file keeps its newest 100 versions, and versions older than 30 days go at startup (prune()).
namespace LocalHistory {

enum class Kind { Saved, AutoSaved, QodeEdit, Refactor, BeforeDelete, BeforeRestore, OnDisk };

struct Version {
    QString file; // the stored copy
    qint64 time = 0; // msecs since the epoch
    Kind kind = Kind::Saved;
};

struct TrackedFile {
    QString path;
    qint64 newest = 0;
    int versions = 0;
    bool exists = true;
};

QString label(Kind kind);
// Saves of the current thread record this kind while a Reason lives (QODE edits, refactors); Saved otherwise.
class Reason
{
public:
    explicit Reason(Kind kind);
    ~Reason();
    Reason(const Reason &) = delete;
    Reason &operator=(const Reason &) = delete;

private:
    Kind m_previous;
};
Kind currentReason();

// Stores `content` as the newest version of `path`, unless it equals the newest stored one.
void record(const QString &path, const QByteArray &content, Kind kind);
// Stores what is on disk now: a file, or every text file under a folder (at most a few thousand; build output and
// dependency folders are skipped).
void recordDisk(const QString &path, Kind kind);
// Around a write: the disk version first (only stored when it is new), then the content written.
void recordWrite(const QString &path, const QByteArray &written, Kind kind = currentReason());

QVector<Version> versions(const QString &path); // newest first
QByteArray content(const Version &version);
QVector<TrackedFile> filesUnder(const QString &folder); // files with versions under the folder, deleted ones too
void pathMoved(const QString &from, const QString &to); // a file or a folder: the versions follow
void prune();
QString storageRoot();

} // namespace LocalHistory
