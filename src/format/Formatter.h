#pragma once

#include <QString>

// "Format Document" / "Format on Save": pipes the text through an external formatter that is installed
// for the file's language (clang-format, prettier, black, gofmt, rustfmt, shfmt, stylua).
namespace Formatter {

struct Result {
    bool ok = false;
    QString text;  // formatted text when ok
    QString error; // why it failed (also filled when no formatter exists)
    QString tool;  // e.g. "clang-format"
};

// True when some installed formatter handles this file type.
bool isAvailable(const QString &filePath);
// Human readable list of the tools that could format this file type, for "not installed" messages.
QString suggestedTools(const QString &filePath);
// Blocking; returns quickly for normal files. `text` uses '\n' line endings.
Result format(const QString &filePath, const QString &text, int timeoutMs = 5000);

} // namespace Formatter
