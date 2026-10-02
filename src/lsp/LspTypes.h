#pragma once

#include <QString>

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
