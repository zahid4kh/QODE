#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

// The conversations Claude Code saved for a folder (~/.claude/projects/<folder with every non-alphanumeric as '-'>/
// <session id>.jsonl, one JSON entry per line). Read only: QODE lists them and shows one again before resuming it.
namespace claudecode::history {

struct Info {
    QString id;
    QString title;    // the title Claude Code gave it, else the first prompt
    QDateTime updated;
};

QString folderFor(const QString &projectRoot);
QVector<Info> list(const QString &projectRoot, int max = 30); // newest first

struct Conversation {
    // The main thread in order: user prompts and assistant / user `message` objects (content blocks: text, thinking,
    // tool_use, tool_result), each as {"role", "content", "toolUseResult"?}. Side chains (subagents) and meta entries
    // are left out. A prompt's content is its typed text; what went with it is in "images" (image `source` objects),
    // "selections" (<ide_selection> blocks) and "files" (project-relative paths of attached `@path` blocks).
    QVector<QJsonObject> entries;
    // The size of the context after the last main-thread reply (its prompt plus output), -1 when not known (none yet,
    // or the conversation was compacted after it).
    int contextTokens = -1;
};
Conversation load(const QString &projectRoot, const QString &id);

// Prompts typed in the terminal for this folder (Claude Code's own ~/.claude/history.jsonl: one {"display", "project",
// "timestamp", …} per line), oldest first, at most `max`. For prompt recall in the input.
QStringList typedPrompts(const QString &projectRoot, int max = 100);

// A text block that attaches a file the way a typed mention does ("@src/a.cpp", `@"with space.txt"`), and back.
QString fileBlock(const QString &relativePath);
QString fileOfBlock(const QString &text); // "" when it is not one

} // namespace claudecode::history
