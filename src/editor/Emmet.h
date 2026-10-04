#pragma once

#include <QString>

// A small Emmet implementation for HTML and JSX: `ul>li.item$*3`, `div#app>header+main^footer`, `a[href=#]{Link}`,
// `(dt+dd)*2`, `input:text`, `!` ... It returns snippet text (`$1`, `$2` tab stops, `$0` at the end) that
// CodeEditor::insertCompletion expands like any LSP snippet.
namespace Emmet {

struct Options {
    bool jsx = false;  // className / htmlFor, `<br />`
    QString indent;    // one indentation level of the file
};

// Column in `line` where the abbreviation that ends at `column` starts, or -1 when there is none that could expand
// (nothing before the caret, inside a tag or an attribute value, a plain word that is not a tag ...).
int findAbbreviation(const QString &line, int column, bool jsx);

// The expansion as snippet text, or an empty string when `abbreviation` is not valid Emmet.
QString expand(const QString &abbreviation, const Options &options);

} // namespace Emmet
