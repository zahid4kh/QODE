#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

// Pure helpers behind keeping language servers in sync cheaply (Qt Core only, no state): the ranged didChange, semantic
// token deltas and the file-operation filters servers register for renames.
namespace LspSync {

// textDocument/didChange content change that turns `before` into `after`: the range between their common prefix and
// suffix (UTF-16 positions in `before`, never splitting a surrogate pair) and the text that replaces it. Several edits
// within one debounce become one change spanning them. Empty object when the texts are equal.
QJsonObject contentChange(const QString &before, const QString &after);

// The server's textDocumentSync change kind: 2 = incremental, otherwise the whole text is sent.
bool wantsIncremental(const QJsonObject &serverCapabilities);

// Applies a SemanticTokensDelta's edits (start, deleteCount, data) to the previous token array. False when an edit does
// not fit the array; it is then left as it was.
bool applyTokenEdits(QVector<quint32> &data, const QJsonArray &edits);
QVector<quint32> tokenData(const QJsonArray &data);

// workspace.fileOperations.<op> registration ({filters: [{scheme, pattern: {glob, matches, options}}]}): does it ask to
// hear about `path`, a folder when `isDir`?
bool fileOperationMatches(const QJsonObject &registration, const QString &path, bool isDir);
// An LSP glob (`**`, `*`, `?`, `{a,b}`, `[a-z]`, `[!a]`) against a whole path.
bool globMatches(const QString &glob, const QString &path, bool ignoreCase = false);

} // namespace LspSync
