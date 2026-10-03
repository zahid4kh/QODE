#pragma once

#include "lsp/LspTypes.h"

#include <QString>
#include <QVector>

// QODE's own snippets, offered in the completion list next to the language server's items. Bodies use the LSP snippet
// syntax ($1, ${2:default}, $0, repeated $1 mirrors); a tab in a body stands for one indentation level.
namespace Snippets {

// Snippets for a language name as Languages::nameForFile returns it ("TypeScript", "C++" ...). Built once per
// language, so asking again is a hash lookup. Empty for languages without any.
const QVector<LspCompletionItem> &forLanguage(const QString &language);

} // namespace Snippets
