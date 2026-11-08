# Windows status (2026-11-08)

The Windows port on `windows-port` is finished and tested on Windows. Only release extras are left (see the end).
The branch is 90 commits ahead of `main`, and `main` hasn't been touched.

## Done and tested on Windows

The build target is Windows 11 x64, built with MSVC 14.51 against Qt 6.11.3 (`msvc2022_64`).

- **Phases 1–8 of `plans/windows-port.md`:**
  - `src/platform/` layer;
  - ConPTY terminal (PowerShell default; cmd and Git Bash optional);
  - paths, encodings and file watchers;
  - language servers, JDKs and Gradle / Maven wrappers;
  - QODE chat and Claude Code view;
  - UI polish: AltGr, dark title bar, tray notifications, scaling;
  - packaging and transparency: installer, portable zip, Help > QODE's Files.
- **Features merged from Ubuntu,** pulled and tried on Windows:
  - **Game panel** (`Ctrl+Alt+P`): Teqris and Snake both play. Scores go to `%APPDATA%\QODE-AGENTIC\games\` and
    are listed in QODE's Files.
  - **Quit crash fix:** QODE quits cleanly, with no leftover `qode.exe`, `java.exe` or `node.exe`.
- **Checked in the user's tests on `D:\deskit` (Kotlin / Gradle):**
  - Kotlin language server installed from QODE;
  - Gradle wrapper upgraded from QODE;
  - run commands, terminal, git and file dialogs work.
- **Installer:**
  - installs per user, without admin rights;
  - the uninstaller lists QODE's data with sizes and deletes what is ticked;
  - QODE never writes the registry. The only registry entries are the installer's own uninstall entry.
- **Since the last pull:** `6ec9d39` fixed an MSVC float warning in `SnakeWidget.cpp`. The colours are unchanged.

## Windows packages built (`dist-win\`, not in git)

The files without "noweb" in the name include WebEngine.

| File | Size | sha256 |
|---|---|---|
| QODE-2.0.0-setup.exe | 110.8 MB | 619eedfb1e725a64daf00ee476d45296181707d890cf42911a35552cae161327 |
| QODE-2.0.0-portable.zip | 153.3 MB | ee795b85a3a2350ff0e1746fb8a6892d474982dcf952635513f2b33f710babe5 |
| QODE-2.0.0-noweb-setup.exe | 34.2 MB | 70e340f77d0e0983034d3a51b5360874c91fd94c457c9fd9cd1b14dd819d8b8a |
| QODE-2.0.0-noweb-portable.zip | 47.3 MB | 4f553c8379d022a8f792affaff4e1e3a302751377442f4249dc08791af984f4e |

To build them again: `powershell -ExecutionPolicy Bypass -File scripts\win\build-all.ps1`.

## Still to do on Ubuntu, before merging

The Linux builds already succeeded with Qt 6.11 and the system Qt 6.4. That was before `6ec9d39`, which only adds
casts.

1. Pull: `git switch windows-port && git pull`.
2. Rebuild:
   - `scripts/compile.sh`
   - `QODE_QMAKE=/usr/bin/qmake6 scripts/compile.sh`
3. Run QODE on Linux. It hasn't been run since the Windows work:
   - **Help > QODE's Files:** the folders with sizes, and the package's files under "Installed with QODE (package)".
   - **File dialogs:** they remember their last folder after a restart. That memory is now in
     `~/.config/QODE-AGENTIC/QtProject.conf`.
   - **Everyday tools:** terminal, git, language servers, QODE chat and the Claude Code view behave as before. A lot
     of Unix code moved into `src/platform/`, and it is meant to be unchanged.
   - **New Project dialog:** the dropdown arrows show.
   - **Games:** they play, and the scores are in `~/.config/QODE-AGENTIC/games/`.
4. Packages: `scripts/build-all-debs.sh`, then install and remove a `.deb` to check it.
5. Merge `windows-port` into `main` and push `main`.

## Later (Phase 8h, not started)

- Code signing for `qode.exe` and the setup files. Without it, SmartScreen warns users when they download them.
- Windows download links and release notes on the website (`docs/`).
- A winget manifest.

`windows-next-steps.md` is the checklist from the previous push. Its Windows steps (pull, build, test, packages) are
all done now.
