#pragma once

#include <QList>
#include <QString>

// Subsequence matcher with editor-style scoring: consecutive runs, word/path boundaries and
// camelCase humps score higher, gaps cost a little, and matches in a file's base name win.
namespace FuzzyMatcher {

// Returns -1 when `pattern` is not a subsequence of `text`, otherwise a score (higher is better).
// `nameStart` is the index where the file-name part of a path begins (0 for plain text).
// When `positions` is given it receives the matched character indexes in `text`.
int score(const QString &pattern, const QString &text, int nameStart = 0, QList<int> *positions = nullptr);

} // namespace FuzzyMatcher
