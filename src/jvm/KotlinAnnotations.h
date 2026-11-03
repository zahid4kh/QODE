#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Completion for Kotlin annotations that need no language server: `@file:` targets (JvmName, OptIn, and in scripts
// DependsOn / Repository / Import / CompilerOptions), common annotations after `@` in files no server analyses, and the
// arguments: Maven coordinates in @file:DependsOn("…") from the local Maven / Gradle caches, repository URLs, scripts
// for @file:Import, opt-in markers. Maven Central hits are added by the caller (they arrive later). Qt Core only.
namespace KotlinAnnotations {

enum class Context { None, At, FileTarget, DependsOn, Repository, Import, OptIn };

struct Where {
    Context context = Context::None;
    QString typed;       // the argument text in front of the word being typed (DependsOn / Repository / Import)
    int argStart = -1;   // column where the string argument starts (after the quote)
};
// `beforeWord`: the line text in front of the word being typed.
Where contextOf(const QString &beforeWord);

struct Entry {
    QString label;
    QString detail;
    QString insert;      // "" = the label
    bool snippet = false;
    int kind = 0;        // LSP CompletionItemKind
    bool replaceArg = false; // replaces the whole argument typed so far (from Where::argStart)
};
// `path`: the file being edited; `served`: a language server analyses it (then only `file:` targets and arguments).
QList<Entry> complete(const QString &path, const Where &where, bool served);

// "group:artifact:version" of every artifact in ~/.m2/repository and Gradle's module cache, newest version each.
// Read once (in a few hundred milliseconds at most), then cached.
QStringList localArtifacts();
// Versions of group:artifact found locally, newest first.
QStringList localVersions(const QString &group, const QString &artifact);

} // namespace KotlinAnnotations
