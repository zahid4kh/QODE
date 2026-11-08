# Windows: next steps (after the 2026-11-08 push of `windows-port`)

## 1. Pull

```bat
cd <repo>
git switch windows-port
git pull
```

`CLAUDE.md` and `scripts\` are now tracked. The pull replaces the laptop's ignored copies without asking. That's fine:
they weren't edited on Windows after being copied to Ubuntu.

Check: `git ls-files --eol scripts/win/compile.bat` should show `w/crlf`.

## 2. Build

```bat
scripts\win\compile.bat
scripts\win\compile.bat noweb
```

Look for compile errors or new warnings in `src\game\*` (only compiled with GCC so far) and in `MainWindow.cpp`.

## 3. Test (in `D:\deskit`)

- **Game panel:** use `Ctrl+Alt+P` or the gamepad button in the toolbar.
  - Play a round of Teqris and one of Snake.
  - The scores should be in `%APPDATA%\QODE-AGENTIC\games\` (`arcade.json`, `teqris.json`, `snake.json`).
- **Help > QODE's Files:** the config entry's description mentions `games\`.
- **Quit without a crash,** with the project open and the Kotlin language server running:
  - close the window with X;
  - File > Exit;
  - `Ctrl+Q`.
  
  After each one, check Task Manager: no `qode.exe` and no stray `java.exe` / `node.exe` left behind.
- **Optional:** clangd and the other language servers you haven't tried yet.

## 4. Packages

```bat
powershell -ExecutionPolicy Bypass -File scripts\win\build-all.ps1
```

Install the setup .exe and run it once. Then uninstall it: the data list should include `games\`, and it should be
removed when ticked.

## 5. Merge (once all of the above is fine)

On Ubuntu: merge `windows-port` into `main`. Local `main` only has the crash fix (`0497145`), which is already on the
branch, so the merge should be clean. Then run `scripts/compile.sh` and `QODE_QMAKE=/usr/bin/qmake6 scripts/compile.sh`, and push
`main`.

If something fails on Windows, fix it on `windows-port`, commit and push, then pull on Ubuntu.
