#pragma once

#include "MoveRefactor.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <memory>

namespace MoveRefactor {

// Collects the edits of one file; an edit that overlaps an earlier one is dropped.
class Emitter
{
public:
    bool add(int start, int length, const QString &text);
    bool covered(int start, int length) const;
    QVector<TextEdit> edits;
    int references = 0;
};

// Shared state of one planning run: the moves, cached file system answers and texts, the emitters.
class Ctx
{
public:
    explicit Ctx(const Input &in);

    const Input &in;
    QString root;
    QStringList movedFiles;      // every file that changes place (folders expanded)
    QSet<QString> movedExts;     // lower-case suffixes of movedFiles
    QStringList notes;
    std::shared_ptr<void> webState, pyState, jvmState, nativeState; // per-language caches, owned by their pass

    // New location of a path (itself when it does not move).
    QString map(const QString &path) const;
    bool moved(const QString &path) const { return map(path) != path; }
    // Where a path that is a destination of a move currently lives (itself when it is not one).
    QString unmap(const QString &path) const;
    const QVector<PathMove> &moves() const { return m_moves; }

    bool isFile(const QString &path) const;
    bool isDir(const QString &path) const;
    bool exists(const QString &path) const { return isFile(path) || isDir(path); }

    // Text of a project file; null when unreadable (binary, too large, not UTF-8).
    const QString &text(const QString &path);
    bool readable(const QString &path);
    Emitter &emitter(const QString &path); // path = location before the move
    const QHash<QString, Emitter> &emitters() const { return m_emitters; }
    void note(const QString &n) { if (!notes.contains(n)) notes.append(n); }

    static QString dirOf(const QString &path);
    static QString nameOf(const QString &path);
    static QString stemOf(const QString &path); // name without the last suffix
    static QString suffixOf(const QString &path); // lower case, without dot
    static bool under(const QString &path, const QString &dir); // path is dir itself or inside it
    static QString relative(const QString &fromDir, const QString &target);

private:
    QVector<PathMove> m_moves; // longest `from` first
    mutable QHash<QString, qint8> m_kind;
    QHash<QString, QString> m_texts;
    QSet<QString> m_unreadable;
    QHash<QString, Emitter> m_emitters;
};

// Per-language passes (each file implements one).
void scanWeb(Ctx &ctx, const QString &file);     // JS/TS, CSS, HTML, Markdown, JSON, PHP, shell (path based)
void scanPython(Ctx &ctx, const QStringList &files);
void scanJvm(Ctx &ctx, const QStringList &files);
void scanNative(Ctx &ctx, const QStringList &files); // C/C++, Go, QML, build files
void scanRust(Ctx &ctx, const QStringList &files);

// Names a text may mention when it refers to a moved path (cheap prefilter).
bool mentions(const QString &text, const QSet<QString> &keys);

} // namespace MoveRefactor
