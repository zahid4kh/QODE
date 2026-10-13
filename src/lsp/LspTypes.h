#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

// One diagnostic of a textDocument/publishDiagnostics notification. Lines are 0-based and columns are
// UTF-16 code units, which is what QString and QTextBlock use too.
struct LspDiagnostic {
    enum Severity { Error = 1, Warning = 2, Information = 3, Hint = 4 };
    int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
    int severity = Error;
    QString message;
    QString source;
    QString code;
    QString server;  // id of the server that reported it (ESLint runs next to the TypeScript server)
    QJsonObject raw; // as sent by the server; code action requests hand it back
};

// A place in a file (0-based line, UTF-16 column), e.g. a definition.
struct LspLocation {
    QString path;
    int line = 0, column = 0;
    bool fromClass = false; // a compiled class mapped to its source file: the line is unknown, look for the declaration
};

// A text change in LSP terms: replace [start, end) (0-based line, UTF-16 column) with `text`.
struct LspTextEdit {
    int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
    QString text;
};

// One entry of a textDocument/completion answer.
struct LspCompletionItem {
    QString label;
    QString signature;  // shown right after the label, e.g. "(int a, int b)"
    QString detail;     // shown at the right edge, e.g. the return type
    QString insertText; // what to insert when there is no textEdit; empty = the label
    QString filterText; // what the typed prefix is matched against; empty = the label
    QString sortText;
    int kind = 0;       // CompletionItemKind (1 Text ... 25 TypeParameter)
    bool snippet = false;
    bool builtin = false; // one of QODE's own snippets (editor/Snippets): matched by prefix only
    bool deprecated = false;
    bool hasEdit = false;
    LspTextEdit edit;                     // replaces the prefix the server saw
    QVector<LspTextEdit> additionalEdits; // e.g. an #include line
    QString command;      // run after accepting (Kotlin: jetbrains.kotlin.completion.apply adds the import and inserts the text)
    QJsonArray commandArgs;
    QJsonObject resolveData; // the item as listed; set when the server resolves items (completionItem/resolve): the details
                             // panel asks for documentation, and accepting it fetches what the list left out (auto-import edits)
    QString documentation;   // Markdown, as listed or after resolve
    QString longDetail;      // multi-line detail after resolve ("Auto import from './utils'\nfunction foo(): void")
    QString server;          // id of the server that listed it
    bool resolveOnAccept = false; // accepting needs the resolved item first (jdtls imports, TypeScript auto-imports)
    bool resolved = false;   // documentation / longDetail are final (the resolve answer arrived)
};

// One entry of a textDocument/codeAction answer (quick fix, refactoring, "organize imports").
struct LspCodeAction {
    QString title;
    QString kind;
    QJsonObject json; // the whole CodeAction; its `edit` and/or `command` are applied when chosen
    QString server;   // which server offered it (and must run its command)
};

// textDocument/signatureHelp: the signatures of the call around the caret and which parameter is being typed.
struct LspSignature {
    QString label;                      // "greet(name: string, times?: number): void"
    QString documentation;              // Markdown
    QVector<QPair<int, int>> params;    // [start, end) of each parameter inside `label`
    QStringList paramDocs;              // Markdown per parameter (may be empty strings)
    int active = -1;                    // this signature's own activeParameter, or -1 for the response's
};
struct LspSignatureHelp {
    QVector<LspSignature> signatures;
    int activeSignature = 0;
    int activeParameter = -1;
    bool isEmpty() const { return signatures.isEmpty(); }
};

// textDocument/documentHighlight: another use of the symbol under the caret (kind 1 text, 2 read, 3 write).
struct LspHighlight {
    int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
    int kind = 1;
};

// textDocument/inlayHint: text drawn inside the code before `column` (kind 1 type, 2 parameter name).
struct LspInlayHint {
    int line = 0, column = 0;
    QString label;
    int kind = 0;
    bool padLeft = false, padRight = false;
};

// A colour literal found by textDocument/documentColor (components 0..1).
struct LspColor {
    int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
    double red = 0, green = 0, blue = 0, alpha = 1;
    QJsonObject raw; // the ColorInformation, handed back to colorPresentation
    QString server;
};

// One way to write a picked colour (textDocument/colorPresentation): "#ff0000", "rgb(255, 0, 0)", ...
struct LspColorPresentation {
    QString label;
    bool hasEdit = false;
    LspTextEdit edit;
};
