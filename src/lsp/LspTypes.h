#pragma once

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
};

// A place in a file (0-based line, UTF-16 column), e.g. a definition.
struct LspLocation {
    QString path;
    int line = 0, column = 0;
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
    bool deprecated = false;
    bool hasEdit = false;
    LspTextEdit edit;                     // replaces the prefix the server saw
    QVector<LspTextEdit> additionalEdits; // e.g. an #include line
};
