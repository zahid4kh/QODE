#pragma once

#include <QString>

// "Copy Reference" for Kotlin / Java: the fully qualified name of what the caret is on, the way IntelliJ writes it
// (com.example.Outer.Inner for a type, com.example.Outer#member for a member, com.example.topLevelFun in Kotlin).
// Text only, no language server: declarations of the file, its imports, then the declaration around the caret.
// Qt Core only.
namespace JvmReference {

bool supports(const QString &path); // .kt / .kts / .java

// `line` / `column` are 0-based. "" when nothing qualifies (the caller falls back to path:line).
QString at(const QString &path, const QString &text, int line, int column);

} // namespace JvmReference
