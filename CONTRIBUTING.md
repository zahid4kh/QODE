# Contributing to QODE

Thanks for your interest in QODE. Bug reports, fixes and features are all welcome. For anything bigger than a small fix, please open an issue first, so we can agree on the approach before you put time into it.

## Build

You need Qt 6.4 or newer (Core, Gui, Widgets, Network; WebEngine, Multimedia and DBus are optional), qmake6, zlib and a C++17 compiler.

### Linux

```sh
scripts/compile.sh            # release build into build/, produces build/qode
build/qode ~/some/project     # try your change
```

`scripts/qt.sh` picks the Qt: `$QODE_QMAKE` if set, else the newest `~/Qt/<version>/gcc_64`, else `qmake6` from `PATH`. Add `CONFIG+=no_webengine` to build without the web preview.

### Windows

Qt 6 for MSVC (`msvc2022_64`) and Visual Studio 2022 or newer:

```bat
set QODE_QT_ROOT=C:\Qt
scripts\win\compile.bat
```

This makes a release build in `build-win\`, with Qt deployed next to `qode.exe`. `QODE_QT_ROOT` is the folder that holds `6.x\msvc2022_64`. Use `scripts\win\compile.bat noweb` to build without the web preview.

The packaging scripts (`scripts/build-*.sh`, `scripts\win\build-installer.ps1`) are only needed for releases.

## Checking a change

There is no test suite yet. Before you open a pull request:

1. `scripts/compile.sh` builds cleanly, **with no new warnings**.
2. The code still builds with **Qt 6.4**, the oldest supported version (Ubuntu 24.04's): `QODE_QMAKE=/usr/bin/qmake6 scripts/compile.sh`. Don't use Qt APIs newer than 6.4, and include the headers you use directly (6.11 pulls in more indirectly).
3. You tried the feature in a running QODE, on a real project.

If you can only test on one OS, say which one in the pull request.

## Code rules

The full list is in [CLAUDE.md](CLAUDE.md). It is written for coding assistants, but it is the most complete reference. The ones that come up most:

- **Add new files to the build by hand:** `.cpp` / `.h` to `SOURCES` / `HEADERS` in [QODE.pro](QODE.pro), resources to [resources/qode.qrc](resources/qode.qrc). There is no globbing.
- **Read the area's notes first.** [dev-docs/](dev-docs/) records why things are the way they are (past bugs, server quirks, pinned versions). If you change behaviour, update the matching file in the same commit.
- **Modules talk through signals** routed by `MainWindow`, never by calling each other directly.
- **Strings:** `QT_NO_CAST_TO_ASCII` and `QT_NO_CAST_FROM_BYTEARRAY` are on, so use `QStringLiteral` / `QLatin1String`. Include headers relative to `src/`.
- **No hard-coded colours.** Add a field to `Theme`.
- **OS-specific code goes in `src/platform/`** behind `Q_OS_WIN` / `Q_OS_UNIX`. Outside it, use the helpers listed in CLAUDE.md (paths, processes, shell commands) instead of `/bin/sh`, `/proc`, `::kill` or `startsWith('/')`.
- **Never write into the user's project folder.** Per-project data goes through `SettingsManager`. Anything QODE writes outside its own folders must be listed in `app/QodeFiles`.
- **User-facing text says "QODE",** never "agent" or "AI".
- **No secrets in prompts.** API keys and secret files (`.env*`, keys, credentials) are never sent to a model.
- **Third-party assets** (icons, fonts, images) need a compatible license and an entry in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Commits and pull requests

- One commit per feature or fix, with a message that says what changed and why.
- Keep pull requests focused. Unrelated clean-ups belong in their own PR.
- Sources use LF line endings; `scripts/win/*.bat` must stay CRLF (`.gitattributes` handles it).
- Using a coding assistant is fine. [CLAUDE.md](CLAUDE.md) is there for it. You are still responsible for understanding and testing what you submit.

## License

By contributing, you agree that your contributions are released under the [MIT License](LICENSE), like the rest of QODE.
