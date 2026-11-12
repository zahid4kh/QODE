#pragma once

#include <QString>

namespace claudecode {

// What QODE adds to every Claude Code process besides its MCP server (QodeMcp): a short system prompt
// (--append-system-prompt-file) and a plugin with QODE's skills (--plugin-dir), which the CLI lists as /qode:<skill>.
// Both ship in resources/claude-code/ and are written to AppPaths::data("claude-code") (never into the project), again
// whenever QODE ships different text. A build without Qt WebEngine leaves out the preview skills and prompt lines.
namespace QodePlugin {

struct Files {
    QString pluginDir;    // for --plugin-dir; empty when it could not be written
    QString systemPrompt; // for --append-system-prompt-file; empty when it could not be written
};

// Writes what differs from the shipped files (cheap when nothing does: called for every new process, so a folder
// removed meanwhile comes back) and returns the paths.
Files install();

} // namespace QodePlugin
} // namespace claudecode
