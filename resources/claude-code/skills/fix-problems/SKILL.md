---
name: fix-problems
description: Fix the errors and warnings QODE knows about (language servers and compiler output in QODE's terminal), checking each fix with the language server instead of a full build. Use when the user asks to fix errors, warnings, red squiggles, "the problems", a failed build in QODE's terminal, or after a large edit that may have broken things.
---

# Fix what QODE lists as wrong

QODE collects problems from two places: the language servers of the files it has open, and compiler errors it read from its own terminal. Its tools see both, and they are much cheaper than a build.

1. **Collect.** Call `mcp__qode__get_problems` (default: errors and warnings for the whole project). Pass `severity: "error"` first when there are many. If the user mentions a failed command, also read `mcp__qode__terminal_output`.
   - Problems only cover files a language server has seen. When the user names a file that is not listed, call `mcp__qode__get_diagnostics` with its path: QODE opens it for its server and returns the current errors.
   - A file under `node_modules`, `build`, `.gradle` or another generated folder is not the user's code: fix the cause, not the file.
2. **Group by cause.** Several errors often share one cause (a renamed symbol, a missing import, a wrong type in one signature). Fix the cause first, earliest file in the chain first.
3. **Fix**, then **check each edited file** with `mcp__qode__get_diagnostics` (one call per `path` you changed). It answers for the text on disk, which your Edit tool has already written.
4. **Repeat** from step 1 until the errors are gone or what is left needs the user (a missing dependency to install, a design choice, a secret). Warnings come after errors; leave warnings alone when fixing them would change behaviour, and say so.
5. **Confirm once at the end** with the project's real check only when it is cheap or the user asked (a type check such as `tsc --noEmit`, `./gradlew compileKotlin`). Do not run a full build after every edit.

Terminal errors (source `terminal`) come from the last command the user ran in QODE's terminal and do not update by themselves: after fixing them, run the same command again with Bash to confirm.

When a language server reports nothing for a file that clearly has errors, `mcp__qode__check_language_servers` says whether one is installed and running for that language; `mcp__qode__install_language_server` installs a missing one (the user approves it).

Finish with a short list: what was wrong, what you changed, what is left and why. `mcp__qode__open_file` can show the user the one place that still needs their decision.
