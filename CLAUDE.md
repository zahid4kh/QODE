# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**QODE** (binary and package `qode`) is a native C++17 / Qt 6 (Core, Gui, Widgets, Network; WebEngine / Multimedia / DBus optional) code editor for Linux with a coding chat built in: project explorer, tabbed syntax-highlighted editor with split groups, LSP, git, debugger and an integrated pty-backed terminal (toggle with `Ctrl+J`). Commit 149c472 brought in the app code (`src/`, `resources/`, `QODE.pro`) from the older QODE-AGENTIC repo (`../QODE-AGENTIC-IDE`), after the old AI chat / agent was removed (ec0b420). This repo is now where all development happens; the other one is no longer used. Key points:

- **Chat-first layout.** The QODE chat (`Ctrl+Alt+G`) is the main view. The editor column opens beside it when files are opened. QODE starts on the welcome page (recent projects) with the chat closed.
- **Two providers.** The chat runs on **Gemini or Claude**, with Claude models called through the Anthropic Messages API. Inline edit (`Ctrl+K`) uses the same providers.
- **Claude Code view.** A second page of the chat area (top bar button, Ctrl+Alt+C) is a graphical front end for the `claude` CLI: install check, sign-in, permission prompts as cards, conversation history, `/usage` and `/mcp`.
- **Called QODE in the UI.** User-visible text never says "agent" or "AI": the top-bar title, reply labels, placeholders, dialogs and the View action "QODE Chat" all use the app name (the Claude Code view is the exception: it is named after the CLI it runs). Internally the code still says agent / ai (`src/agent`, `src/ai`).
- **Data folders.** Package, binary, desktop entry and icons are `qode`, but settings, data and caches live under `QODE-AGENTIC` (`~/.config/QODE-AGENTIC`, `~/.local/share/QODE-AGENTIC`, `~/.cache/QODE-AGENTIC`), not the older `QODE` folders.

API keys live only in mode-0600 files or the environment and are never put into prompts. Secret files (`.env*`, keys, credentials, `claude.key`/`gemini.key`) are never sent to the model. There are no third-party dependencies and no test suite or linter configured.

Git-ignored local material (may be absent): `dev-docs/` (the per-area notes linked below), `plans/`, `PRD.md`, `whats-new`, `run.sh`, `run.bat`, `scripts/` (Linux build / packaging scripts and `scripts/win/`), `bump-version.sh`, `publish-apt.sh`, `.env.local`, and API reference pages in `g-docs/` (Gemini) and `claude-api-docs/` (Anthropic Messages API). Check those pages before guessing request/response shapes in `AgentRunClaude.cpp` / `ClaudeClient` or the Gemini half. `scripts/` is git-ignored too (kept local, not public); there is no CI: the user builds the Debian packages locally with `scripts/build-all-debs.sh`.

## Build & run

- `scripts/compile.sh` — release build into `build/` (produces `build/qode`) with the Qt chosen by `scripts/qt.sh` (see below)
- `./run.sh [project-dir | file ...]` — runs `build/qode`, building only if it is missing (it does not rebuild after source changes — rerun `scripts/compile.sh` first)
- Manual: `mkdir build && cd build && ~/Qt/6.11.3/gcc_64/bin/qmake6 ../QODE.pro && make -j$(nproc)`; `sudo make install` (PREFIX defaults to `/usr/local`) installs binary, desktop entry and icon.
- Three Debian packages (DEBIAN scripts, man pages, changelog and `qode-remove-lsp` live in `packaging/`; what they share is in `scripts/deb-common.sh`, the coloured output in `scripts/ui.sh`). Each conflicts with and replaces the other two, and switching between them keeps the user data:
  - `scripts/build-deb.sh [--no-build]` — `qode_<version>_<arch>.deb`, dynamically linked against the system Qt 6.4 (Depends on the distro's qt6 packages), built in `build-deb/` with `PREFIX=/usr`, staged in `staging_qode/`.
  - `scripts/build-deb-qt.sh [--no-build]` — `qode-bundled_<version>_<arch>.deb`: Qt 6.11 with Qt WebEngine bundled under `/opt/qode` (needs `patchelf`; only runs on the same or newer glibc).
  - `scripts/build-deb-qt-noweb.sh [--no-build]` (= `build-deb-qt.sh --no-webengine`) — `qode-bundled-noweb_<version>_<arch>.deb`: the same without Qt WebEngine (`CONFIG+=no_webengine`): no web / Expo preview, no Chromium, a much smaller package.
  - `scripts/build-all-debs.sh [--no-build]` builds all three into `debs/`.
  - `packaging/debian/*` hold `@PACKAGE@` / `@VERSION@` / `@FLAVOR@` / `@WEB@` placeholders that `deb_install_scripts` fills in: postinst prints the version, install / upgrade / switch, the Qt flavour, whether the preview is in it and the changelog notes of this version.
- `./bump-version.sh <x.y.z|patch|minor|major> [-m "note" ...] [-n]` bumps `VERSION` everywhere (QODE.pro, `docs/` package.json and download links, new top entry in `packaging/changelog`); `-n` is a dry run.
- `docs/` is the unrelated marketing landing page (Next.js 16 / React 19 / Tailwind 4, pnpm): `cd docs && pnpm dev` / `pnpm build`.

Build artifacts (`build*/`, `staging_qode*/`, `debs/`, `.deb` files) are git-ignored.

With no tests, the way to check a change is `scripts/compile.sh` (a clean build with no new warnings), then `./run.sh <project>` to try the feature. When testing the Claude Code view against the real `claude` CLI, use tiny prompts and as few runs as possible.

### Which Qt is used

The scripts build against Qt 6.11.3 from `~/Qt/6.11.3/gcc_64` (installed with `aqtinstall`, WebEngine 6.11 = a recent Chromium). The system Qt is Ubuntu's 6.4.2 (`qt6-*` apt packages; WebEngine = Chromium 102); it stays installed and is only used by `build-deb.sh`.

- `QODE.pro` does not choose Qt: the qmake that configures the build does, and the binary finds that Qt's libraries through the RUNPATH qmake adds (`ldd build/qode | grep Qt6` shows which Qt is loaded).
- [scripts/qt.sh](scripts/qt.sh) (sourced by the build scripts) sets `QMAKE`: `$QODE_QMAKE` if set, else the newest `~/Qt/<version>/gcc_64/bin/qmake6`, else `qmake6` from `PATH`. A build folder configured by a different qmake is wiped and configured again, so switching Qt never mixes objects. `QODE_QMAKE=/usr/bin/qmake6 scripts/compile.sh` builds against the system Qt.
- `compile.sh` (and so `run.sh`; extra arguments go to qmake, e.g. `CONFIG+=no_webengine`) and `build-deb-qt.sh` use that Qt. `build-deb-qt.sh` bundles it under `/opt/qode`: the Qt libraries plus everything else in Qt's own `lib/` (ICU, FFmpeg), the plugins, and (not with `--no-webengine`) WebEngine's `QtWebEngineProcess` (`libexec/`), `resources/` and `translations/qtwebengine_locales`, with `qt.conf` pointing at them. The package needs the same or a newer glibc than the build machine.
- `build-deb.sh` always uses `/usr/bin/qmake6`: its package depends on the distro's `qt6` packages, which a Qt from `~/Qt` would not match.
- So the code must also build with Qt 6.4: no API newer than 6.4 (e.g. `QRegularExpression::matchView` is 6.5), and include headers that 6.11 pulls in indirectly but 6.4 does not (`QWindow` for `focusWindowChanged`, `QGuiApplication`, the full type behind a `QPointer<T>` member in a QObject header). `QODE_QMAKE=/usr/bin/qmake6 scripts/compile.sh` or `scripts/build-deb.sh` checks it.
- Qt 6.11 deprecation warnings (`invalidateFilter`, `globalMatch`, unchecked `QFile::open`) are known and harmless. Version-sensitive code worth re-checking after a Qt upgrade: the SVG `<mask>` workaround in `webdev/WebPreview.cpp` and anything documented as a Qt 6.4 / Chromium 102 quirk in `dev-docs/`.

### Windows (branch `windows-port`)

The plan is [plans/windows-port.md](plans/windows-port.md). Linux / Unix code is never deleted: Windows gets a second implementation (the `src/platform/` layer, see the plan), and Linux must still build with Qt 6.11 and the system Qt 6.4.

- **Machine:** Qt 6.11.3 at `D:\qt\6.11.3\msvc2022_64` (WebEngine, Multimedia, bundled zlib via `QT += zlib-private`). Visual Studio Community 2026 at `D:\vs-studio-18` (MSVC 14.51, Windows SDK 10.0.26100), which builds fine against the `msvc2022_64` kit (same ABI). jom if installed, else nmake with `/MP`.
- **Build:** `scripts\win\compile.bat [noweb]` builds a release into `build-win\` (`build-win-noweb\` with `CONFIG+=no_webengine`): it finds qmake (`%QODE_QMAKE%`, else the newest `D:\qt\6.*\msvc2022_64`, root overridable with `%QODE_QT_ROOT%`) and MSVC (vswhere → `vcvars64.bat`) by itself. Extra qmake args must be quoted (`"DEFINES+=X"`): cmd splits arguments at `=`.
- **Deploy:** `compile.bat` ends with `windeployqt` (Qt DLLs and plugins copied next to `qode.exe`, incremental), so the exe also starts with a double-click; `set QODE_NODEPLOY=1` skips it.
- **Test project:** try features in `D:\deskit` (Kotlin / Gradle), not in this repo. All Windows phases are done (only Phase 8h, signing / release links / winget, is left); the branch is now being checked on Linux (Qt 6.11 and the system Qt 6.4) before the user merges it.
- **Run:** `run.bat [noweb] [nobuild] [paths] [-n]` in the repo root: incremental build, then starts `qode.exe` with Qt's `bin\` on PATH for that process. Relative paths are made absolute.
- **Two Windows builds only**, both with Qt bundled (windeployqt + installer, Phase 8): with WebEngine and `noweb`. No dynamically linked / system-Qt flavour on Windows.
- **Packages:** `scripts\win\build-installer.ps1 [-NoWeb] [-NoBuild] [-NoPortable]` stages `qode.exe`, Qt and the MSVC runtime DLLs in `staging-win\`, then builds `dist-win\QODE-<version>[-noweb]-setup.exe` with Inno Setup 6 ([packaging/windows/qode.iss](packaging/windows/qode.iss)) and a portable zip; `scripts\win\build-all.ps1` builds both flavours. The installer is per user (`%LOCALAPPDATA%\Programs\QODE`, no admin), both flavours share one AppId, and the uninstaller lists QODE's data through `qode.exe --qode-files list|remove` (`app/QodeFiles`). QODE holds the mutex `QODE-AGENTIC-running` so setup can ask to close it.
- **Shell:** this is Windows. Use PowerShell or cmd; Git Bash (the Bash tool) only for POSIX scripts. `bash.exe` on PATH is the WSL stub with no distro, so Linux builds cannot be checked here: the user checks them on Ubuntu. Never run destructive removals (build folders, files, git resets): say what and why, and wait for a yes.
- **Git:** one commit per feature or fix, on `windows-port` only. Never merge into `main`; the user merges after testing everything. The repo sets `core.autocrlf=input`, so sources stay LF. `.bat` files must be CRLF.
- **Windows code rules:** OS-specific code lives in `src/platform/` behind `Q_OS_WIN` / `Q_OS_UNIX`, not scattered `#ifdef`s. Outside it, never use `/bin/sh`, `split(':')` on PATH (use `QDir::listSeparator()`), `QDir::homePath() + "/.cache/…"`, `::kill`, `/proc`, or `startsWith('/')` as an absolute-path test. Do not `#include <windows.h>` outside `src/platform/` (and define `NOMINMAX` / `WIN32_LEAN_AND_MEAN` there).
- **Platform APIs to use instead** (all in `src/platform/`, Unix halves return exactly the old values):
  - `AppPaths::config/data/cache(sub)`: QODE-AGENTIC folders.
  - `ProcessTree::track/stop/kill`: long-running children.
  - `Platform::setShellCommand(proc, line)`: shell line.
  - `Platform::setCommand(proc, program, args)`: any tool that may be a `.bat`/`.cmd`.
  - `Platform::wrapperCommand/wrapperFile/wrapperNeedsExecBit(dir, "gradlew"|"mvnw")`.
  - `Platform::venvBinDir/venvProgram`.
  - `Platform::gitExecutable()`.
  - `Platform::extraBinDirs()`.
  - `Platform::hasExecBit()`.
  - `Platform::restartSelf()`.
  - `Platform::processCwd(pid)` / `Platform::shellInForeground(pid)` (no `/proc` outside `src/platform/`), `Platform::terminalShells()`.
  - `Platform::exeSuffix/pythonCommand/bashProgram`: words for default run commands (Unix: "", python3, bash).
  - `Platform::qmakeCandidates/qmakeScopes/qmakeRunCommand`: qmake projects (Windows: Qt installer kits, a Run `.bat` with vcvars64 / MinGW).
  - `ProcessTree::stop(QProcess*)` for servers whose input QODE does not use (answers cmd's "Terminate batch job").
  - `setShellCommand` takes an sh line; on Windows it is translated for cmd (`ShellSyntax`).
  - Paths:
    - `pathKey` / `samePath` / `isUnder` (never `==` / `startsWith(root + '/')` between two file paths: Windows ignores case);
    - `uriToPath` / `pathToUri` for `file://` URIs (servers send `c%3A`);
    - `invalidFileName` for names users type;
    - `decodeOutput` for child-process output (not `fromLocal8Bit`);
    - `hiddenByOs`, `legacyEncoding`, `busyFileRetries`, `canMakeSymlinks`.
  - Processes and files (Phase 5):
    - `killTree(proc)` to stop a short-lived tool started with `setCommand` (a `.bat` / `.cmd` leaves java / node running otherwise);
    - `removeTree(path)` for every recursive delete (Qt's `removeRecursively` follows junctions on Windows);
    - `realPath` (follows junctions), `setCurrentVersion` / `currentVersion` (managed installs' `current`: symlink / junction), `commandLinkFile` / `writeCommandLink` / `commandLinkTarget`, `setUnpackCommand` (System32 tar.exe), `setRemoveCommand` / `removeCommandText`, `osTag()` ("linux" / "win");
    - tools: `jdkFolders` / `jdkHomes`, `pythonInterpreter` (py, never the Store stub), `gitBashExecutable`, `languageServerDirs` (LLVM, MSYS2), `systemIncludeDirs`, `hasMsvc`, `templateValues` (project templates' Windows values).
  - Project templates: `{{os}}`, a "win" object in template.json overrides keys there, options with `"os": "!win"` are hidden (fixed value); Windows files are `compile.bat` / `run.bat` (written CRLF).
  - Text files are decoded and encoded with `editor/TextEncoding` (UTF-8, UTF-8 BOM, UTF-16 LE / BE, Latin-1, Windows-1252), never `fromUtf8` on a user's file.
  - A rename or move of a folder holds a `WatchGuard::Scope` (Windows cannot rename a folder with anything watched below it); a new long-lived watcher registers with `WatchGuard::registerWatcher` (a plain `QFileSystemWatcher` plus its re-arm function) or `registerOwner`.
  - Terminal: the pty is `ShellProcess` (Unix) / `platform/ConPty` (Windows). Command lines typed into the terminal are written for sh and go through `Terminal::runCommand`, which rewrites them for PowerShell / cmd / Git Bash (`terminal/ShellSyntax`); a line typed into a fresh shell by the startup provider is written per shell (`ShellSyntax::dialectOf` / `quote` / `sequence`).
  - Helpers run by `main` (`--qode-ctrl-c`, `--qode-restart-after`) go in `runIfWatchdog` / `runIfHelper`.

## Code rules

- Build is qmake-only ([QODE.pro](QODE.pro)); it errors out on Qt < 6. **Every new .cpp/.h must be added to `SOURCES`/`HEADERS` in QODE.pro by hand** (no globbing), and new resources (fonts, icons, template files) to [resources/qode.qrc](resources/qode.qrc).
- `QT_NO_CAST_TO_ASCII` / `QT_NO_CAST_FROM_BYTEARRAY` are on: use `QStringLiteral`/`QLatin1String` for string literals passed as QString.
- Headers are included relative to `src/` (e.g. `#include "editor/Document.h"`).
- Qt Multimedia (`QODE_HAS_MULTIMEDIA`, video in the media panel), Qt WebEngine (`QODE_HAS_WEBENGINE`, the embedded live web preview; `CONFIG+=no_webengine` leaves it out, and then there is no web / Expo preview at all) and Qt DBus (`QODE_HAS_DBUS`, desktop notifications in `app/DesktopNotify`) are optional: guard code that uses them with those defines so the build still works without the module. zlib (`-lz`) is required by `lsp/JarSource`. `QODE_VERSION` comes from `VERSION` in QODE.pro.
- Never hard-code a colour in a widget: add a field to `Theme` (see [dev-docs/app.md](dev-docs/app.md)).
- Nothing is ever written inside the user's project folder; per-project data lives in `~/.config/QODE-AGENTIC/projects/<name>-<hash>.json` through `SettingsManager`.
- Attach documents to a `CodeEditor` with `attachDocument()`, never plain `setDocument()` (it renders in the UI font).
- File / folder pickers: `FileDialogs::getOpenFileName` etc. (`dialogs/FileDialogs.h`), never the `QFileDialog` statics: on Windows Qt writes `HKCU\Software\QtProject` whenever a dialog closes, and the wrapper puts it back.
- Nothing QODE writes may land outside its own folders (`AppPaths`) unless it is listed in `app/QodeFiles` (Help > QODE's Files and the Windows uninstaller show that list): transparency about what QODE creates is a product goal.
- The Claude agent history must stay append-only within a run (thinking blocks are bound to the exact prefix) — see [dev-docs/agent.md](dev-docs/agent.md).

## Architecture

`main.cpp` creates `Application` (QApplication subclass; theme, fonts, pointing-hand cursors) and a `MainWindow`, then calls `openInitialPaths` with CLI args.

`MainWindow` (`src/window`) is the composition root. It owns the managers (`ProjectManager`, `EditorManager`, `LspManager`, `GitRepository`, `MoveController`, …) and all menu/toolbar `QAction`s, wires their signals together and persists the session through `SettingsManager`. Modules under `src/` talk to each other only through Qt signals routed by `MainWindow`, never directly. Layout: left `SideSections` panel (Explorer / Version Control / Search / Tasks) | `m_workSplit` (agent chat as the main view, editor column beside it) | right-hand island (media/preview, Gradle, Spring panels), with the terminal panel at the bottom.

| Area | Where | Notes |
| --- | --- | --- |
| Window, layout, settings, themes, icons, windows, dialogs, project templates, the watchdog that stops servers when QODE dies, startup time (Help > Startup Time, panels created on first use) | `window`, `settings`, `app`, `dialogs`, `project/ProjectTemplates` | [dev-docs/app.md](dev-docs/app.md) |
| Editor: documents, split groups, save flow, highlighting, `CodeEditor` extras, multiple cursors, Ctrl+click links, code lenses (▶ Run / Debug over Java / Kotlin tests and mains, server lenses such as reference counts), `.editorconfig`, local history (versions on save / before QODE and refactor edits / before delete, diff and restore), palette, search, formatter, bookmarks, CSV table, media/Markdown preview, Copy as Image (code pictures to share); C/C++ conveniences (Switch Header/Source, `#include` completion, new-file guards, Generate Definition, smart `->`, terminal compiler errors in Tasks > Problems) | `editor`, `history`, `palette`, `search`, `format`, `tasks`, `media`, `cpp`, `window/MainWindowCpp.cpp`, `window/MainWindowEditing.cpp`, `window/MainWindowLens.cpp` | [dev-docs/editor.md](dev-docs/editor.md) |
| Explorer drag-and-drop / rename with import fix-ups | `explorer/ExplorerTree`, `refactor` | [dev-docs/refactor.md](dev-docs/refactor.md) |
| Language servers: registry, installers, companions, assists, semantic data, call / type hierarchy, code lens requests, server status and progress, responsiveness | `lsp`, `window/MainWindowLsp.cpp`, `window/MainWindowAssist.cpp`, `editor/CodeEditorAssist.cpp` | [dev-docs/lsp.md](dev-docs/lsp.md) |
| JVM run configs, Gradle tasks and task timings, the Maven panel (lifecycle, plugin goals, profiles, dependency tree, effective POM), Spring Boot, Ktor (server bar, auto-reload, panel with log / routes read from the running server over JDWP / requests / config, plugins); Java / Kotlin: Kotlin scripts (F5, kotlinc check), ▶ on main functions, package fix, dependency manager (Maven Central), project JDK, version status, New Kotlin / Java class dialog, Copy Reference, scratch files, gradle.properties and `@file:` annotation completion; the JDWP debugger (breakpoints, steps, variables) | `project/JvmProject`, `gradle`, `maven`, `spring`, `ktor`, `jvm`, `debug`, `window/MainWindowKtor.cpp`, `window/MainWindowJvm.cpp`, `window/MainWindowJvmEdit.cpp`, `window/MainWindowMaven.cpp`, `window/MainWindowDebug.cpp` | [dev-docs/jvm.md](dev-docs/jvm.md) |
| JS/TS dev server, `.env` editor, Expo | `webdev`, `expo` | [dev-docs/web.md](dev-docs/web.md) |
| HTTP client: `.http` / `.rest` files (variables, environments, chaining), the panel, the shared response view | `http`, `window/MainWindowHttp.cpp` | [dev-docs/http.md](dev-docs/http.md) |
| Python server, venvs, packages | `python`, `project/PythonEnv` | [dev-docs/python.md](dev-docs/python.md) |
| QODE chat (agent): providers, tools, diagnostics after edits, usage meter (tokens, cache hits, cost), codebase index (semantic_search), journal undo/redo, slash commands, MCP servers (stdio client, config, dialog, /mcp), Fix with QODE (a failed terminal command or test, Tasks > Problems) | `agent`, `ai`, `mcp`, `window/MainWindowFix.cpp` | [dev-docs/agent.md](dev-docs/agent.md) |
| Inline edit box (Ctrl+K, `/?`, "Ask about this to make a change"): one streamed Gemini / Claude request, the proposal live in the text, Accept / Reject | `ai/InlineEdit`, `ai/InlineClient`, `window/MainWindowInline.cpp` | [dev-docs/editor.md](dev-docs/editor.md) |
| Claude Code view: `claude` CLI over stream-json, sign-in, permissions, history, /usage and /mcp cards | `claudecode` | [dev-docs/claude-code.md](dev-docs/claude-code.md) |
| Terminal (own pty + VT emulator, shell integration for exit codes) and git (shells out to `git`) | `terminal`, `git`, `window/MainWindowGit.cpp` | [dev-docs/terminal-git.md](dev-docs/terminal-git.md) |
| Game panel (right-hand island, `Ctrl+Alt+P`): a picker with Teqris and Snake, shared drawing in `GameArt`, scores and options per game in `AppPaths::config("games/<id>.json")` (`GameStore`) | `game`, `window/MainWindowGame.cpp` | — |

`project` (`ProjectManager`, `ProjectModel`, `ProjectFiles` index), `explorer` (`ProjectExplorer`) and `filesystem` (`FileManager` create/rename/delete helpers) are small enough to read directly.

Read the matching `dev-docs/` file before changing a module; it records non-obvious decisions (why things are pinned, past bugs, server quirks). Update it whenever behaviour changes (it is git-ignored here, so it is not part of the commit).
