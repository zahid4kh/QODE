---
name: debug
description: Debug a Java or Kotlin program with QODE's debugger (breakpoints, stepping, variables, evaluating expressions) to find out why it misbehaves at run time. Use when a JVM program, test or Gradle / Maven app gives a wrong result, crashes or hangs and the cause is not obvious from the code, instead of adding print statements.
---

# Debug with QODE's debugger

QODE has a JDWP debugger for Java and Kotlin. Its tools set breakpoints the user sees in the editor, start the program the way the user's Debug button (Shift+F9) does, and read the stopped program's frames and variables. Use them instead of adding print statements and running the program again.

1. **Pick the lines.** Read the code and choose where the wrong value first shows up: the start of the suspect function, the line before a crash in a stack trace, the loop that computes the result. Lines are 1-based and must hold code (not a blank line, comment or a lone brace).
2. **Set breakpoints** with `mcp__qode__debug_breakpoints` (`action: "add"`, `path`, `line`), one call per line. `action: "list"` shows what is set, including the user's own breakpoints: leave those alone.
3. **Start** with `mcp__qode__debug_start`:
   - `path`: a file with a `main` function (the one nearest the top runs) or a `.kts` script;
   - no `path`: the Gradle / Maven project's run configuration.
   It builds, runs the program in QODE's terminal and answers at the first stop with the reason, the stack (file:line) and the top frame's variables. The user approves the start. A build counts against `wait_seconds` (default 120): for a big Gradle project pass more. If it answers that the program ended without stopping, the breakpoint is not on a line that runs: read `mcp__qode__terminal_output` and pick another line.
4. **Look around.**
   - `mcp__qode__debug_state` with `frame` shows another frame's variables; `object_id` lists an object's fields or a collection's elements.
   - `mcp__qode__debug_evaluate` evaluates an expression in a frame (`list.size()`, `a + b`, `user.name`).
   - `mcp__qode__debug_step` with `over` (default), `into`, `out` or `continue` (to the next breakpoint) answers at the next stop.
5. **Stop** with `mcp__qode__debug_stop` as soon as you know the cause, and remove the breakpoints you added (`action: "remove"` per line, or `clear` with the `path` when the file had none of the user's).
6. **Fix** the code, then run the program or test again with Bash (or `debug_start` once more) to confirm.

A program that waits for input on stdin, loops forever or never reaches a breakpoint does not stop: `debug_start` answers after `wait_seconds` with the state "running". Read `mcp__qode__terminal_output`, then `debug_step` with `continue` or `debug_stop`.

Tell the user what you found in terms of the code: the line, the value that was wrong and why, not the debugger steps you took.
