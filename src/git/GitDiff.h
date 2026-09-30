#pragma once

#include <QStringList>
#include <QVector>

// Line-based diff (Myers O(ND)) used by the editor gutter and the diff viewer.
namespace GitDiff {

// A changed region. Line indexes are 0-based; a count of 0 means "pure insertion" / "pure deletion".
struct Hunk {
    int oldStart = 0, oldCount = 0;
    int newStart = 0, newCount = 0;
    bool isAdded() const { return oldCount == 0; }
    bool isDeleted() const { return newCount == 0; }
};

// Splits on '\n' (after normalising CRLF), keeping a trailing empty entry so that it matches
// QTextDocument's block structure.
QStringList splitLines(const QString &text);

QVector<Hunk> compute(const QStringList &oldLines, const QStringList &newLines);

} // namespace GitDiff
