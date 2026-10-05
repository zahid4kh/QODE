#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// Works out which text edits keep a project consistent when files or folders are moved: import statements, include
// directives, package declarations, relative links, build-file entries ... Pure computation (no GUI, thread safe);
// the caller applies the edits.
namespace MoveRefactor {

struct PathMove {
    QString from, to; // absolute paths; a file or a folder
};

struct TextEdit {
    int start = 0;  // offsets into the text the plan was computed on (QString units)
    int length = 0;
    QString text;
};

struct FileChange {
    QString path;    // where the file lives after the move
    QString oldPath; // where it lives now
    QVector<TextEdit> edits; // ascending, never overlapping
    int references = 0;      // statements rewritten
    size_t baseHash = 0;       // qHash of the text the offsets refer to (detects edits made meanwhile)
};

struct Plan {
    QVector<FileChange> changes;
    QStringList notes; // things the person should look at
    int references() const;
};

struct Input {
    QString root;
    QVector<PathMove> moves;
    QStringList files;             // every project file (absolute), as they are before the move
    QHash<QString, QString> texts; // unsaved editor contents, they win over the disk
};

Plan plan(const Input &in);

// File helpers shared with the code that applies a plan (UTF-8 only; false for binary / other encodings).
bool readText(const QString &path, QString *text, bool *bom = nullptr);
bool writeText(const QString &path, const QString &text, bool bom);

QString applyEdits(const QString &text, const QVector<TextEdit> &edits);
// Edits that turn applyEdits(text, edits) back into text.
QVector<TextEdit> inverse(const QString &text, const QVector<TextEdit> &edits);

} // namespace MoveRefactor
