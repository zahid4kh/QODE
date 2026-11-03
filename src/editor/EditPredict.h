#pragma once

#include <QString>
#include <QVector>
#include <functional>

// Next edit predictions, worked out from the text alone (no model): after the user changed a piece of code, the other
// places in the file the same change belongs to.
//  - Rename: an identifier (or a name with one qualifier, `self.count`, `console.log`) became another one -> its other
//    uses. A renamed parameter or local variable only changes inside its function.
//  - Parameter list: a parameter was added to or removed from a function declaration -> its calls get the parameter's
//    name inserted as a placeholder argument, or lose the matching argument.
namespace EditPredict {

// The user's edit: [start, start + after.size()) of the current text was `before` when the edit began.
struct Change {
    int start = 0;
    QString before, after;
};

struct Edit {
    int start = 0, end = 0; // range of the current text that is replaced
    QString text;
    int selectFrom = -1, selectLength = 0; // part of `text` to select once applied (a placeholder), -1 for none
};

enum class Kind { None, Rename, AddArgument, RemoveArgument };

struct Prediction {
    Kind kind = Kind::None;
    QString from, to; // Rename: old and new name. Arguments: the function name, and the parameter's name.
    QVector<Edit> edits; // ascending, not overlapping
};

struct Context {
    QString language;                   // Language name from Language.cpp ("Python", "C++", ...), empty when unknown
    std::function<bool(int)> inString;  // offset of a character inside a string literal
    std::function<bool(int)> inComment; // offset of a character inside a comment
};

Prediction predict(const QString &text, const Change &change, const Context &context);

} // namespace EditPredict
