# Kotlin LSP in QODE: plan

Status: **implemented** (2026-10-02). Decisions taken: install under `~/.local/share/QODE/lsp/`, create the `~/.local/bin/kotlin-lsp` symlink by default, pin a tested version (263.4702.0) with "latest" as an opt-in checkbox.

Verified while implementing: the standalone launcher runs with no Java on PATH (it uses its bundled `jbr/`, Java 25), `--stdio` is the stdio flag, no `--eula` is needed, `--system-path` sets the cache, `--version` prints `263.4702.0-EAP` in under a second (also through a symlink), unpacked size is 1.2 GB. The server uses pull diagnostics and only analyses Gradle/Maven projects ("no supported build system found" otherwise). Not verified: diagnostics from a real Gradle import (no Gradle on the dev machine).

Original plan below, kept for reference.

## 1. Which download is the right one?

Neither the VSIX nor the "zip + kotlin-lsp.sh" from the README. Use the **standalone Linux archive** from the GitHub release notes.

| Option | Verdict |
|---|---|
| `.vsix` (VS Code extension) | Same server inside a VS Code wrapper (a zip with an `extension/` folder). Works, but it is VS Code packaging and the layout is not meant for other editors. Avoid. |
| **Standalone archive** `kotlin-server-<ver>.tar.gz` (Linux x64) / `-aarch64.tar.gz` | **Use this.** It is the build "for editors other than VS Code". |
| README "download the zip" | There is no `.zip` for Linux; zips are Windows only. Linux is `.tar.gz`. The README wording is stale. |
| `kotlin-lsp.sh` from the repo `scripts/` folder | Not needed. The archive already contains it, and it prints a deprecation warning and just execs `bin/intellij-server`. |

Download URLs (these are not on GitHub itself; the GitHub release has no assets, only links to download.jetbrains.com):

- x64: `https://download.jetbrains.com/language-server/kotlin-server/<VER>/kotlin-server-<VER>.tar.gz` (about 368 MB)
- arm64: `.../kotlin-server-<VER>-aarch64.tar.gz`
- checksum: same URL plus `.sha256` (format `<hex> *<filename>`)

Archive layout (checked): `kotlin-server-<VER>/{bin/intellij-server, bin/intellij-server.vmoptions, kotlin-lsp.sh, lib/, jbr/, build.txt}`.

### Java is NOT needed

The archive ships its own JetBrains Runtime in `jbr/` (that is a big part of the 368 MB). So the "check Java, prompt to install 17+" step in the original idea is unnecessary for the standalone archive. Does the launcher use `jbr/`? Not yet read: I could not get `bin/intellij-server` out of the tarball, so confirm after the first download (open the script, or run it with a PATH that has no `java`). If it turns out to need `JAVA_HOME`, set `JAVA_HOME=<install>/jbr` in the `QProcess` environment instead of asking the user to install Java. Only fall back to the Java-17 prompt if that fails.

## 2. Install location and "symlink"

Do the setup under QODE's own data dir, not `$HOME` root:

- Install dir: `~/.local/share/QODE/lsp/kotlin/<VER>/` (`QStandardPaths::AppLocalDataLocation`-style), extracted from the tar.
- Stable pointer: `~/.local/share/QODE/lsp/kotlin/current` symlink to the version dir (makes updates atomic and rollback trivial).
- PATH symlink (optional, off by default): `~/.local/bin/kotlin-lsp` pointing at `current/bin/intellij-server`. QODE does not need it, because it stores the absolute path in `SettingsManager::lspServerPath("kotlin")`. Offer it as a checkbox ("also add `kotlin-lsp` to ~/.local/bin") for users who want it in a terminal.
- Make executable: `bin/intellij-server`, `jbr/bin/*` (tar preserves modes, so this should already be set; verify and `chmod +x` if not).
- No sudo anywhere.

## 3. Automated "Download & Set Up" flow

Entry: LSP menu > Kotlin > **Download & Set Up…** (also offered from the status-bar hint when a `.kt` file is opened and no server is found).

1. **Confirm dialog**: shows version, about 370 MB download, about 800 MB unpacked (check this after the first extract), destination folder, and that it is downloaded from download.jetbrains.com. User clicks *Download*.
2. **Resolve version**: query `https://api.github.com/repos/Kotlin/kotlin-lsp/releases/latest`, take `tag_name` (`kotlin-lsp/v263.4702.0`), strip to `263.4702.0`. On failure (offline, rate limit) fall back to a pinned version constant in `LspServers` and say so.
3. **Pick arch**: `QSysInfo::currentCpuArchitecture()` gives `x86_64` or `arm64`. Anything else shows "not supported on this CPU, download manually".
4. **Download** with `QNetworkAccessManager` (the Network module is already linked), streamed to `<dir>/kotlin-server-<VER>.tar.gz.part`, with a progress bar and Cancel. Follow redirects. Check free disk space first (`QStorageInfo`).
5. **Verify** SHA-256 against the `.sha256` file using `QCryptographicHash` (stream-hash while downloading). Mismatch aborts and deletes the file.
6. **Unpack** by running `tar -xzf <file> -C <tmp-dir>` through `QProcess` (tar exists on every Linux, and there is no libarchive dependency). Extract into `<VER>.tmp/` and rename to `<VER>/` when done, so a half-extracted install never looks valid.
7. **Finalize**: `chmod` where needed, update the `current` symlink, optionally create the `~/.local/bin` symlink, delete the tarball, store the path in `SettingsManager`, and remove older version dirs only after the new one passes the smoke test.
8. **Smoke test**: start the server through `LspClient` (initialize handshake). Success shows "Kotlin language server is running (<serverInfo.version>)". Failure shows the last log lines and keeps the install dir for debugging.
9. Everything runs on the event loop with async `QNetworkReply` / `QProcess`, so the UI never blocks (same pattern as `GitRepository`).

Put this logic in its own class, `src/lsp/LspInstaller` (download, verify, extract, finalize; signals `progress`, `finished`, `failed`), plus a small `dialogs/LspInstallDialog` for the progress UI. `MainWindow` wires the menu action to it. Add both to `QODE.pro` by hand.

Principle check: QODE still never bundles a server. It downloads only when the user clicks the button, from the vendor, with a visible confirmation. Update the README line "QODE does not ship any language server" to mention the optional installer.

## 4. Registering Kotlin as a server (`LspServers`)

New `LspServerSpec`:

- id `kotlin`, extensions `kt`, `kts` (`java` is out of scope)
- executables: `kotlin-lsp`, `kotlin-lsp.sh`, `intellij-server` (PATH lookup), plus the QODE-managed path stored in settings
- args: **stdio mode**. Check `intellij-server --help` after install; the likely flag is `--stdio`. The Neovim doc mentions a socat/netcat workaround, which suggests the default may be a socket, so confirm this before coding.
- install help text: link to the GitHub release page, plus "or click Download & Set Up"
- root detection: the server wants a project root with `settings.gradle(.kts)` / `build.gradle(.kts)` / `pom.xml`. Single loose `.kt` files get limited support ("single file support" is not guaranteed). `LspManager::startServer` currently uses the project root, else the file's folder, which is fine. Surface a status hint when no build file is found.

## 5. Client work (capabilities and behaviour the Kotlin server may need)

Likely small, but verify against the server's actual behaviour:

- The server is slow to start (JVM + Gradle/Maven import, 10 s to minutes on first run). The status bar and LSP menu must show "Starting / Indexing…" instead of looking frozen. Handle `$/progress` and `window/workDoneProgress/create` (already answered with null) and show the title/percentage in the status bar.
- Completion: it should work as is (we advertise no snippets, plaintext docs). Kotlin items may carry `additionalTextEdits` (auto-imports), which the existing path already applies.
- Hover returns Markdown with `kotlin` code fences. The renderer should be checked for this.
- Diagnostics: standard `publishDiagnostics`. May arrive late after import, which is fine.
- Restart policy: keep "restart a crashed server twice". JVM servers can OOM, so show the last log lines on failure.
- `workspace/configuration` is answered with nulls. If the server wants settings, add Kotlin defaults in the spec.

## 6. UI changes

- LSP menu gains a **Kotlin** section (same pattern as clangd: state, install help, set path, restart, log) and the new **Download & Set Up…** and **Check for Updates** entries. The existing shared `QMenu` is rebuilt on `aboutToShow`, so this is a per-server section loop.
- Status-bar hint for `.kt` files without a server: "Kotlin language server not installed: Download & Set Up".
- Update dialog text for a missing/old install; "Remove Kotlin language server" deletes the managed dir.

## 7. Risks and open questions

1. **Size**: about 370 MB download is big; make the size explicit in the dialog. Resume support is optional.
2. **Alpha server**: JetBrains marks it Alpha and partly closed source. Add a one-line note in the dialog. License terms: confirm redistribution is not involved (we download straight from JetBrains, we do not rehost), and consider showing the license link.
3. **Launcher flags**: stdio flag and JVM memory options (`bin/intellij-server.vmoptions`) are unverified.
4. **Gradle import**: the server runs Gradle/Maven itself and needs network and a JDK for the *project* being built (that is separate from the bundled `jbr/`). A project that needs JDK 21 may fail with a confusing message; capture and show server log lines.
5. **Android / Kotlin Multiplatform** projects: experimental or unsupported per README. Do not promise them.
6. **Version pinning vs latest**: auto-latest can break us when JetBrains changes flags. Pin a known-good version as default, with "latest" as an advanced option.
7. **Domain stability**: `download.jetbrains.com/language-server/kotlin-server/...` is taken from the official release notes. If the layout changes, the version resolver breaks, so keep a manual "Set Server Path…" fallback.

## 8. Implementation order (one commit each, as usual)

1. `LspServerSpec` for Kotlin plus menu section, install help text (links only, no installer). Testable manually by downloading the tar once by hand.
2. Verify launcher: stdio flag, `jbr/` use, startup time. Fix spec args from reality.
3. Progress reporting in the status bar (`$/progress`), needed to make a slow server usable.
4. `LspInstaller` + `LspInstallDialog` (download, sha256, tar, symlink, smoke test).
5. Update/remove actions, README + CLAUDE.md updates.

## 9. Decisions needed from you

- Install under `~/.local/share/QODE/lsp/` (recommended) or `$HOME` as you first described?
- Create the `~/.local/bin/kotlin-lsp` symlink by default, or make it an opt-in checkbox (recommended: opt-in)?
- Pin a version (recommended) or always take the latest release?
