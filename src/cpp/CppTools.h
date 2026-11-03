#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// C / C++ helpers that work on plain text (Qt Core only): header <-> source pairs, the text of new files, the `#include`
// being typed, and the out-of-line definition of a declared function (Generate Definition).
namespace CppTools {

bool isHeader(const QString &path);
bool isSource(const QString &path);
inline bool isCpp(const QString &path) { return isHeader(path) || isSource(path); }

// The matching source for a header or header for a source, best first: the same folder, then files of `projectFiles` with
// the same base name, those whose folder shares the longest path with `path` first.
QStringList counterparts(const QString &path, const QStringList &projectFiles);

// Starter text for a new file. A header gets `#pragma once`, or an include guard named like the guards of the project's other
// headers when those use guards; a source file includes the header of the same name next to it. Empty for other files.
QString newFileText(const QString &path, const QString &projectRoot);

// --- #include completion ---------------------------------------------------------------------------------------------------
// `lineBeforeCaret` is the line up to the caret. True when the caret is inside the <...> or "..." of an include; `opener` gets
// '<' or '"' and `typed` what is between the opener and the caret.
bool includeContext(const QString &lineBeforeCaret, QChar *opener = nullptr, QString *typed = nullptr);
// True when the line up to the caret is `#include` (plus spaces): a '<' typed there gets its '>'.
bool atIncludeOpener(const QString &lineBeforeCaret);
// The compiler's own header folders (/usr/include, libstdc++ ...), found once.
QStringList systemIncludeDirs();
struct PathEntry {
    QString name;
    bool dir = false;
};
// The entries of `subfolder` (what was typed up to its last '/') inside each of `folders`: folders and header-like files,
// without duplicates or hidden entries.
QVector<PathEntry> includeEntries(const QStringList &folders, const QString &subfolder);

// --- Generate Definition ---------------------------------------------------------------------------------------------------
struct Declaration {
    bool ok = false;
    QString error;           // why there is nothing to generate, when not ok
    QString name;            // f, operator==, ~Widget
    QStringList classes;     // enclosing classes, outermost first, with their template arguments ("Box<T>")
    QStringList namespaces;  // enclosing named namespaces, outermost first
    QString head;            // template lines + return type, everything in front of the qualified name
    QString tail;            // "(params) const noexcept" without defaults and override / final
    bool keepInFile = false; // templates, constexpr, inline, anonymous namespaces: the definition stays in this file
    int classEnd = -1;       // position after the outermost class's `};` (or the declaration when there is no class)
    int scopeEnd = -1;       // position of the `}` closing the declaration's namespace, -1 at file scope
    QStringList before;      // functions declared in the same scope before / after it, nearest first
    QStringList after;
    QStringList paramTypes;  // normalised parameter types, to tell overloads apart
};
// The function declaration (ending in ';') at `position` of a C/C++ file.
Declaration declarationAt(const QString &text, int position);

struct Insertion {
    bool exists = false; // a definition is already there: `position` is where it starts
    int position = 0;    // where `text` goes (the start of a line)
    QString text;
    int caret = 0;       // offset in `text` of the empty line inside the body
};
// Where the definition goes in `target` (the source file, or the declaring file itself when `sameFile`) and what to insert:
// after the definition of the function declared before it, else before the one declared after it, else at the end of the
// matching namespace block or the file. Namespaces not open at that point are added to the name. `indent` is one level.
Insertion insertionFor(const Declaration &d, const QString &target, bool sameFile, const QString &indent);

} // namespace CppTools
