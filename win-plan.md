# QODE on Windows: plan

Status: **draft for discussion**. No code has been changed. Branch: `windows` (from `main` at the 0.9.0 + CSV commit).
Everything below comes from a read of the current sources; file names are real, the effort numbers are guesses.

---

## 1. Branching (decided)

One codebase. `windows` is an **integration branch**, not a second product.

| Branch | Role |
|---|---|
| `main` | **Stays Linux-native.** Nothing from `windows` goes into it until you have verified the Windows app yourself. Linux releases keep coming from `main` with `build-deb.sh`. |
| `windows` | Branched from `main`, so it already holds all of `main`'s code. All Windows integration happens here. Pushed to the remote so you can pull it on your Windows laptop. |
| `bundled-qt` | Linux-only, local, never pushed, unrelated to this port. |

Rules:

1. **`windows` → `main` happens once, at the end**, only after you confirm on Windows that the app runs and the three
   LSPs (C/C++, TypeScript, Python) set up and work. Until you say so, there is no merge in that direction. Not for
   "safe" early pieces either.
2. If `main` receives new Linux commits meanwhile, they flow **`main` → `windows`** (merge main into windows) so the
   branch does not drift. Never the other way.
3. Every change made here must keep Linux behaviour identical (Windows code is added next to the Linux code, behind
   `#ifdef Q_OS_WIN` or a small platform helper), so the final merge is not a rewrite of Linux paths.
4. After the merge the `windows` branch has no further purpose and can be deleted.

## 2. Target and toolchain

- **Qt 6.5+ LTS** (6.8 LTS preferred). Ubuntu 24.04 gives 6.4, but Windows has no system Qt anyway: we ship our own.
- **Compiler**: MSVC 2022 (the official Qt binaries) *or* MinGW 64 (also official). Pick **MSVC** unless you want to
  build with Qt Creator + MinGW; ConPTY and the Windows SDK headers are easier there. (D2)
- **Build system**: stay on **qmake** for now. It works on Windows (`qmake -tp vc` / `jom`). CMake is not required for
  the port; moving to it is a separate decision.
- **zlib**: `LIBS += -lz` in QODE.pro does not exist on Windows. Qt already ships zlib internally, so either use
  `QtZlib` via `QT += core-private` (`<zlib.h>` from Qt's bundled copy), or vcpkg/`zlib` DLL. Easiest: on `win32`
  link the private Qt zlib or vendor a tiny `miniz`. (D3)
- **Dev environment (decided)**: I write the code on Ubuntu; you clone `windows` on your Windows laptop, build and test
  there. I cannot compile or run anything Windows-specific from here, so expect build-error round trips.
- **CI (later, phase 11)**: GitHub Actions `windows-latest` + `jurplel/install-qt-action` can run the same build script
  and upload the installer. It reuses `build-win.ps1`, so it is added after the local build works (§8).

---

## 3. What is Linux-specific today (audit)

Counted from the sources. Grouped by how hard it is.

### 3.1 Blocking: the app will not build or run

| Area | Where | Problem |
|---|---|---|
| zlib link | `QODE.pro` (`-lz`), `lsp/JarSource` | no system zlib |
| Terminal | `terminal/ShellProcess.cpp` | `posix_openpt`, `grantpt`, `setsid`, `TIOCSCTTY`, `TIOCSWINSZ`, `SIGHUP`. The `#else` branch already returns "only supported on Unix-like systems" so it *compiles* but the terminal is dead. Needs **ConPTY**. |
| Child process groups | `webdev/DevServer.cpp` | `setsid()` + `kill(-pid, SIGTERM/SIGKILL)` to stop the dev server and all its children. Needs a **Job Object** (or `taskkill /T /F`). Also launches via `/bin/sh -c`. |
| Symlink swapping | `lsp/LspInstaller.cpp`, `lsp/JdtlsInstaller.cpp` (`replaceSymlink`, `::symlink`, `::rename`, `unistd.h`) | POSIX calls; symlinks need admin / Developer Mode on Windows. |
| Local sockets | `app/InstanceRegistry.cpp` | `QLocalServer` works on Windows (named pipes) but the "stale socket" logic differs; likely fine, must test. |

### 3.2 Behavioural: compiles, but does the wrong thing on Windows

| Area | Where | Problem |
|---|---|---|
| Data/cache paths | `LspServers.cpp`, `JarSource.cpp`, `LspManager.cpp`, `ExpoSchema.cpp`, `DevServer.cpp`, `SettingsManager`, `QmakeProject.cpp` | Hard-coded `~/.cache/QODE`, `~/.local/share/QODE`, `~/.local/bin`, `~/.config/QODE` (20+ sites in `LspServers.cpp` alone). Must go through one helper over `QStandardPaths` (`%LOCALAPPDATA%\QODE`, `%APPDATA%\QODE`). |
| Executable names | everywhere a program is looked up | `node` → `node.exe`, `npm` → **`npm.cmd`**, `uv.exe`, `python.exe`/`py.exe`, `gradlew.bat`, `mvnw.cmd`, `basedpyright-langserver.exe`, `.cmd` shims for node tools. `QProcess` cannot start a `.cmd` directly, it needs `cmd /c`. `QStandardPaths::findExecutable` already honours `PATHEXT`; many sites use manual `QFileInfo(dir + "/node")`. |
| Venv layout | `project/PythonEnv.cpp`, `lsp/PipInstaller.cpp` | `bin/python`, `bin/activate` → Windows has `Scripts\python.exe`, `Scripts\Activate.ps1`. `isExecutable()` bit checks are meaningless for scripts. |
| Executable bit | `LspInstaller` (`makeExecutable`), `GradleTasks` (`isExecutable()` on `gradlew`) | Windows has no exec bit; use `.bat` / `.cmd` variants. |
| Shell | `ShellProcess::defaultShell` (`$SHELL`, `/bin/bash`), `MainWindow` (`(cd '...' && cmd)`), `Terminal::runCommand` | Default must be PowerShell (`pwsh.exe`, then `powershell.exe`), `cmd.exe` as fallback. Command typing, `cd` syntax, quoting (`'` vs `"`) and `&&` differ per shell. See §5.2. |
| Run commands | `RunConfigDialog::expand`, `JvmProject`, `QmakeProject::runCommand`, `WebProject`, `ExpoBar`, defaults for each file type | Built-in defaults are POSIX (`./gradlew`, `sh gradlew`, `python3 {file}`, `./main`, `g++ … && ./a.out`). |
| Archives | `LspInstaller`, `JdtlsInstaller` | Shell out to `tar -xzf`. Windows 10+ ships `bsdtar` as `tar.exe`, so `.tar.gz` *does* work, but the Kotlin server needs its Windows archive (`.zip`, different name, `.exe`/`.bat` launchers). |
| Removal | `dialogs/LspRemoveDialog.cpp` | Builds visible `rm` / `rm -rf` steps and shows `apt`/`pacman` commands for clangd. Windows: delete in-process (`QDir::removeRecursively`) and show `winget`/`choco`/`scoop` instead. |
| "underHome" safety check | `LspRemoveDialog.cpp` | Uses `/` separators and `startsWith("/usr/")`. Needs `QDir::cleanPath`/`toNativeSeparators`-aware comparisons and case-insensitivity on Windows. |
| Case sensitivity & separators | refactor passes, `ProjectFiles`, Move refactor, search, git status paths | Paths are compared as strings in many places. Windows is case-insensitive and `QDir::toNativeSeparators` appears nowhere; git and LSP URIs use `/`. Rule: **store everything with `/`, convert only when showing the user or calling a native API.** |
| Line endings | `editor/Document.cpp` | Already handles CRLF, good. New files should default to CRLF? (D5) |
| Clipboard / open-in-file-manager | `xdg-open`-style calls in `MainWindow`/`ExpoBar` if any | Use `QDesktopServices::openUrl` / `explorer /select,`. |
| Window chrome | `Application` | Title bar follows system theme; dark title bar needs `DWMWA_USE_IMMERSIVE_DARK_MODE` via `QWindow` attribute or the `windows:darkmode=2` platform argument. |

### 3.3 Cosmetic / nice-to-have

- Fonts are embedded already (Plus Jakarta Sans, JetBrains Mono): no change.
- HiDPI: Qt 6 handles it; check icon sizes.
- App icon: `.ico` + `RC_ICONS` in QODE.pro (`qode-source.png` → `.ico`).
- `.desktop` file, hicolor icons, man pages, `qode-remove-lsp`: Linux-only, simply not installed on Windows.
- File associations / "Open with QODE" shell entry: installer's job (§8).
- Global menu bar, Wayland, `xcb`-specific code: none found, good.

### 3.4 Already portable (no work)

Editor, syntax highlighting, folding, minimap, find/replace, Emmet, snippets, CSV table, themes, settings JSON,
`LspClient` (pure `QProcess` + `Content-Length` framing), the diagnostics/completion/hover logic, project search,
fuzzy palette, git (shells out to `git` which exists on Windows via Git for Windows), markdown/SVG preview, media
panel, the whole refactor engine (pure Qt Core, **but see path-case caveat above**), project templates, Expo schema.

---

## 4. LSP: download / setup / removal on Windows (the big one)

**Scope for now: C/C++ (clangd), TypeScript and Python only.** The other rows are kept as reference for later.
**Rule: QODE never downloads tools it does not own** (clangd, Node, Python, uv). It detects them and shows a dialog
with the install command (e.g. `winget install LLVM.LLVM`) and a Copy button. Only things QODE installs into its own
private folder on the user's click (npm packages, the private Python venv) stay, as on Linux. One Windows reality per server:

| Server | Linux today | Windows approach |
|---|---|---|
| **clangd** | system package (`apt`); QODE only locates it | Detect `clangd.exe` (PATH, `C:\Program Files\LLVM\bin`, configured path). If missing, a dialog shows `winget install LLVM.LLVM` (copy button); no download by QODE. Removal = a dialog with `winget uninstall LLVM.LLVM`. Needs MSVC/MinGW include flags (clangd on Windows uses `--query-driver` or a compile database). |
| **Kotlin** (`kotlin-lsp`) | `kotlin-server-<ver>.tar.gz`, SHA-256, `tar`, `current` symlink, `~/.local/bin` link | Same download host, **Windows build is a `.zip`** with `bin\intellij-server.bat`. Unzip in-process (we already have a zip reader in `JarSource`, reuse it, no `tar`). Replace the symlink with a plain `current.txt` pointer file or just a directory rename/junction. No PATH link (or add the folder to *user* PATH, optional). Verify the pinned SHA for the Windows artifact. Windows ARM64 later. |
| **jdtls** (Java) | tarball + `java -jar plugins/org.eclipse.equinox.launcher_*.jar -configuration config_linux` | Already avoids the Python wrapper, which is the main Windows trap. Needs: `config_win` instead of the platform config dir, `.tar.gz` extracted via bsdtar or in-process, `java.exe` discovery (`JAVA_HOME`, `Program Files\Eclipse Adoptium`, `Microsoft\jdk-*`, `winget`), no symlink. |
| **TypeScript / HTML / CSS / JSON / ESLint / Tailwind** | `npm install --prefix …/lsp/web`, scripts with `#!/usr/bin/env node` | `npm` is **`npm.cmd`**, launchers in `node_modules\.bin` are `.cmd` files. Run the server as `node <js entry>` instead of the shim to avoid `cmd /c` quoting hell. Node discovery: `PATH`, `%ProgramFiles%\nodejs`, `%APPDATA%\nvm`, `%LOCALAPPDATA%\fnm_multishells`/`Volta`, `%USERPROFILE%\.volta`, scoop. Note `npm install` of >1 package may be slow due to Defender. |
| **Python** (`basedpyright` + `ruff`) | private venv, `uv` else `python3 -m venv`; `bin/python` | `Scripts\python.exe`; interpreter discovery via `py.exe -3`, `python.exe`, Microsoft Store stub (`WindowsApps\python.exe` is a **fake launcher**, it must be detected and rejected). `uv.exe` is `winget install astral-sh.uv`. Launchers: `Scripts\basedpyright-langserver.exe`, `Scripts\ruff.exe`. |
| **Remove** | `LspRemoveDialog` runs visible `rm` steps; shows apt/dnf/pacman | In-process recursive delete with the same visible, ordered step list (keep the UX: "Remove the command link / Delete folder / Delete cache"), but implemented with `QDir::removeRecursively`. Files can be **locked** by a running server or Defender: stop the server first, retry with backoff, and report "still in use". Package-manager hints become `winget uninstall` / `choco` / `scoop`. |

Design to make this tractable instead of sprinkling `#ifdef`s:

- A new **`platform/Platform` module** (Qt Core only) with: `dataDir()`, `cacheDir()`, `configDir()`,
  `exeName("node")`, `findExecutable(name, extraDirs)`, `venvBin()`, `venvPython()`, `isExecutableFile()`,
  `makeExecutable()`, `extractArchive(path, dest)` (zip + tar.gz, in-process, no `tar`), `replaceLink(target, link)`
  (symlink on Unix, pointer/junction on Windows), `removeTree()`, `killTree(QProcess*)`, `defaultShell()`.
- `LspServerSpec` gains per-OS fields: `executables` per platform, archive name per platform/arch, config dir name.
- Every installer calls Platform, none calls `tar`, `rm`, `::symlink` directly.

Order inside this phase: Platform helper → **Node servers (typescript/html/css/json)** → **Python** → **clangd** →
**Kotlin** → **Java**. Node servers first because they exercise `.cmd` handling, which everything else depends on.

---

## 5. Terminal, run configurations, dev servers

### 5.1 Terminal (ConPTY)

- Replace the POSIX branch with `CreatePseudoConsole` (Windows 10 1809+). Two pipes in/out, `STARTUPINFOEX` with
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`, `ResizePseudoConsole` for resizes, `ClosePseudoConsole` + `TerminateProcess`
  on exit.
- ConPTY output is a normal VT stream, so **`TerminalScreen` (the escape-sequence emulator) is reused as is**. Expect
  work on the quirks: it emits its own cursor positioning, `\e[?9001h` (win32-input-mode), and `\e]0;title` OSC.
- Keep `ShellProcess`'s public interface (`start/resize/write/output/terminate/pid`); add `ShellProcess_win.cpp`
  and let QODE.pro pick the file per platform. Blocking reads: use a reader thread posting to the Qt event loop (no
  `QSocketNotifier` on pipes on Windows).
- Default shell order: `pwsh.exe` → `powershell.exe` → `cmd.exe`. Native Windows shells only.
- Env: set `TERM_PROGRAM=QODE`; do not set `TERM` (not used by PowerShell).
- Fallback if ConPTY is missing (Windows < 1809): "terminal not supported" message as today.

### 5.2 Run configurations & commands

**Decided:** on Windows QODE does not type commands into the shell for the user. Run / F5 shows the resolved command
(with `{file}` etc. expanded and quoted for PowerShell) in a small panel/dialog with a **Copy** button, and the user
pastes it into the terminal. That removes the need for shell-aware command *generation* in most places. The analysis
below is kept for when typing-into-shell is added back.

The run command is a **string typed into the user's shell**. That model is simple on Linux and fragile across shells.
Options:

- **A (recommended): keep "type a command into the shell"**, but make QODE's own *generated* commands shell-aware.
  A small `ShellKind {Posix, PowerShell, Cmd}` derived from the terminal's program, with helpers:
  `quote(path)`, `cdAndRun(dir, cmd)`, `and(a, b)` (`&&` works in PowerShell 7 and cmd, **not** in Windows
  PowerShell 5.1: use `;` / `if ($?)`), `envVar(NAME, value)`.
- B: run configs as structured `{program, args, cwd, env}` executed with `QProcess` into a terminal. Better long-term
  but a large UI/storage change. Not for this port.

What changes under A:

- Defaults in `RunConfigDialog`, `JvmProject::runCommand` (`./gradlew` → `.\gradlew.bat`, `sh gradlew` fallback
  removed), `GradleTasks::runner`, `MainWindow` `(cd '…' && …)` wrapper (used by Expo/Gradle/dev server), the Python
  activation command (`PythonEnv::activationCommand` → `.venv\Scripts\Activate.ps1`; PowerShell execution policy
  may block it, so prefer running `.venv\Scripts\python.exe` directly or `-ExecutionPolicy Bypass` for that one call).
- `{file}`/`{dir}` expansion quoting follows `ShellKind`.
- Per-OS defaults per file type (Python `py`/`python`, C/C++ `cl`/`g++` depending on what's installed, Rust/Go/Node unchanged).
- `QmakeProject::runCommand` assumes `build/` and a bare binary: add `.exe` and `release\`/`debug\` subdirs for MSVC.
- Expo: `npx expo …` is `npx.cmd`; iOS button stays disabled (not macOS).

### 5.3 Dev server (`webdev/DevServer`)

- Replace `/bin/sh -c` with `cmd /c` (or PowerShell, per §5.2).
- `setsid` + `kill(-pid)` → **Job Object** with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so stopping the server (and
  closing QODE) kills the whole tree (`node`, `vite`, `esbuild`…). `taskkill /PID <pid> /T /F` is the quick fallback.
- Port detection by parsing stdout is unchanged. `$PORT` → `set PORT=…` / `$env:PORT`, but we already pass the
  environment through `QProcessEnvironment`, so keep doing it that way.

---

## 6. Filesystem & path hygiene

Must be done **first and everywhere**, because it breaks silently:

1. Internal paths always use `/` (Qt does this already via `QDir`/`QFileInfo`). Never build with `"\\"`.
2. Native separators only for display and for command strings given to a shell.
3. Windows paths: drive letters (`C:/…`), UNC (`//server/share`), `file:///C:/…` URIs for LSP (`QUrl::fromLocalFile`
   is correct; hand-built `"file://" + path` is not: grep for it).
4. Case-insensitivity: any `==` / `startsWith` / `QSet<QString>` keyed by path (open documents, `InstanceRegistry`
   hash, bookmarks, refactor maps, git status maps) gets a `Platform::pathKey(path)` that lowercases on Windows.
5. Drive letter case differs between sources (`c:` vs `C:`): normalize.
6. Reserved names (`CON`, `NUL`, trailing dots/spaces), 260-char `MAX_PATH` (enable long paths in the app manifest;
   `node_modules` hits this fast).
7. Files in use cannot be deleted/renamed (editor tab open, language server indexing). Explorer rename/delete and
   the Move refactor need retry + friendly error.
8. Symlinks: not available by default, any feature relying on them (`current` links, kotlin-lsp `~/.local/bin`
   link) is rewritten (§4).
9. `FileManager`, `ProjectExplorer` context menu: "Reveal in Explorer" (`explorer.exe /select,"path"`), "Open in terminal here".
10. `QFileSystemWatcher` has a handle limit on Windows (and holds directory locks); `EditorManager` watches individual
    files: fine, but check the number of watched folders in the explorer.

---

## 7. Git on Windows

- Everything shells out to `git`; it exists on Windows only if Git for Windows is installed. Detect `git.exe` (PATH,
  `%ProgramFiles%\Git\cmd`), show a clear "Install Git" hint with a `winget install Git.Git` copy button.
- `--literal-pathspecs`, `status --porcelain=v2 -z`, `blame --porcelain --contents -`: all fine on Windows git.
- `core.autocrlf`: working trees will usually be CRLF. The gutter diff (`git show HEAD:<file>` vs the editor
  buffer) compares LF-normalized text in the buffer against git's output: must normalize both sides before diffing or
  every line shows as modified. **High risk, test early.**

---

## 8. Packaging and distribution

Equivalent of the `.deb`:

| Piece | Plan |
|---|---|
| Deployment | `windeployqt --release --no-translations` against `qode.exe` → folder with Qt DLLs, plugins (`platforms/qwindows.dll`, `imageformats`, `multimedia` if used), MSVC runtime (`vc_redist`). |
| Installer | **Inno Setup** produces a `setup.exe` (simple, scriptable, uninstall script removes files); **WiX** produces a real `.msi`. Open question O2. `build-win.ps1` (the analogue of `build-deb.sh`) builds, deploys and runs `iscc`. |
| Installed layout | `%ProgramFiles%\QODE\` (or per-user `%LOCALAPPDATA%\Programs\QODE`, no admin needed), Start Menu shortcut, optional desktop icon, "Open with QODE" context-menu entry and `qode` on PATH (checkbox), file associations optional. |
| Uninstall | Removes the install dir. **Asks** whether to delete user data (`%APPDATA%\QODE` settings/projects, `%LOCALAPPDATA%\QODE` LSP servers + caches). Mirrors what `postrm purge` does on Linux. |
| Portable | Optional `.zip` of the deployed folder (no installer). A `portable` marker file next to the exe switches data to `.\data`. |
| Signing | Unsigned installers trigger SmartScreen. Code-signing certificate later (SignPath for OSS, or Azure Trusted Signing). |
| Updates | Out of scope. |
| Version | `bump-version.sh` additionally patches the Inno script and the Windows `VERSIONINFO`/`.rc`. |
| Single instance | `InstanceRegistry` over named pipes: verify. |
| Qt inside the app | The user does **not** need Qt installed in either variant. **Bundled DLLs** (`windeployqt`): a folder with `qode.exe` + Qt DLLs + plugins, the Windows analogue of the `bundled-qt` branch (the Linux patchelf/RPATH parts do not apply). **Truly static**: one `qode.exe`, needs a static Qt built from source first (hours), and LGPL compliance gets harder. Open question O1. |
| Build | Phase 10: `build-win.ps1` on your Windows laptop (Qt + MSVC installed there). Phase 11: GitHub Actions runs the same script, Qt installed by `install-qt-action`. The laptop is the build environment for development; Actions is the repeatable one for releases. |

Also: `QODE.pro` gets a `win32 { RC_ICONS = …; QMAKE_TARGET_COMPANY …; CONFIG += windows; … }` block, `unix {…}` keeps the
current `target`/`INSTALLS`; `-lz` becomes `unix:` only.

---

## 9. Phases and status

`[ ]` not done, `[~]` in progress, `[x]` done. A phase is "done" only when you have checked it on Windows.

| Status | # | Phase | Result |
|---|---|---|---|
| [x] | 0 | Agree on this plan (§10 open questions) | decisions written down |
| [~] | 1 | `Platform` helper module; route every data/cache/config path through it (Linux paths unchanged) | no `~/.cache`, `~/.local` literals left |
| [ ] | 2 | Compile and start on Windows: zlib, POSIX includes, `.pro` `win32` block, executable-name helper | window opens; editor, explorer, git, search work; terminal reports "not available" |
| [ ] | 3 | Path hygiene (§6): case/separator/URI audit, git CRLF normalization | explorer, search, refactor and git gutter behave |
| [ ] | 4 | ConPTY terminal (§5.1) with PowerShell / cmd | typing, resize, Ctrl+C, colours work |
| [ ] | 5 | Run configurations on Windows (§5.2): show the command to copy/paste | F5 / Run shows the right command per project type |
| [ ] | 6 | LSP: C/C++ (clangd) | detect, guide dialog, start, diagnostics, "remove" instructions |
| [ ] | 7 | LSP: TypeScript | Node detection, user-started install, start, diagnostics, remove |
| [ ] | 8 | LSP: Python (basedpyright + ruff) | interpreter detection, private venv, start, diagnostics, remove |
| [ ] | 9 | Polish: dark title bar, icon, HiDPI, Reveal in Explorer, long-path manifest | feels native |
| [ ] | 10 | Build + packaging on your Windows laptop (§8): `build-win.ps1`, installer | working `exe` / installer |
| [ ] | 11 | GitHub Actions builds the same artifacts (§8) | artifact on every push to `windows` |
| [ ] | 12 | Your verification pass on Windows (checklist below) | sign-off |
| [ ] | 13 | Merge `windows` → `main` (only after 12) | |

Not in scope for now: Kotlin, Java, HTML/CSS/JSON, ESLint, Tailwind servers, Expo, Gradle/Maven run presets,
dev-server bar. Their code stays as it is and is simply not wired up on Windows (see §4 and §5.3 for how they'd be
handled later).

### Manual checklist (no test suite exists)

Open a project → edit/save → Find in Files → git status/gutter/diff/commit → terminal (type, resize, a full-screen
TUI, Ctrl+C) → Run on a Python and a C++ file → set up each of clangd, TypeScript, Python, open a file, see
diagnostics, then remove it and confirm the folder is gone → Move a file in the explorer with the refactor → close and
reopen (session restore). Run the same list on Linux before the final merge.

---

## 10. Decisions

### Answered

| # | Decision |
|---|---|
| D1 | One codebase; `windows` is an integration branch; `main` stays Linux-native until you verify Windows (§1). |
| D4 | Test on your own Windows laptop. `windows` is pushed to the remote; you clone and build there. No VM. |
| D6 | Native Windows only. No other shell environments. Terminal: PowerShell 7 → Windows PowerShell → cmd. |
| D7 | Commands are not typed into the shell for the user; QODE shows them with a Copy button (see §5.2). |
| D10 | First LSPs: C/C++, TypeScript, Python only. |
| D11 | QODE does not download tools it does not own (clangd, Node, Python, uv). It detects them and shows a dialog with the install command. |
| D2 | Qt ships as **bundled DLLs** (`windeployqt`); the user needs no Qt installed. |
| D3 | Installer is a **`setup.exe`** (Inno Setup), not an `.msi`. |
| D4b | Compiler: **MSVC 2022**. |
| D5 | QODE may run `npm install` / `pip install` when the user clicks "Set up", but only into QODE's own folder. Node and Python are only detected. |
| D8 | The ConPTY terminal is built (phase 4). |
| D9 | The dev-server toolbar strip is hidden on Windows for now. |
| D12 | New files get LF line endings on Windows too; existing files keep their own. |
| D13 | Per-user install, no admin (`%LOCALAPPDATA%\Programs\QODE`). |

### Open

None. Phase 0 (agreeing on this plan) is complete.

---|---|---|
| O5 | Terminal panel (needs the Windows "ConPTY" API) is in scope. What should the dev-server toolbar strip do on Windows for now? | Hidden |
| O6 | New files: LF or CRLF line endings? (Existing files always keep their own.) | LF |
| O7 | Per-user install (no admin prompt) or per-machine? | Per-user |

---

## 11. Risks

All verified by you on Windows before any merge to `main`.

- ConPTY quirks may need `TerminalScreen` fixes that also touch Linux behaviour: keep those fixes guarded and re-test Linux.
- Locked files during LSP removal and the Move refactor.
- Defender scanning `node_modules` slows installs.
- Path-case bugs are silent and numerous.
- Linux development is easier than Windows: mitigated by §1 (nothing reaches `main` early, Linux code paths stay unchanged).

---

## 12. First steps once this plan is agreed

1. Answer the open questions in §10.
2. Push `windows` to the remote (when you say so).
3. Phase 1: `src/platform/Platform.{h,cpp}`, add to `QODE.pro`, migrate the literal paths. **Stays on `windows`.**
4. Phase 2 until the app starts on your Windows laptop.
