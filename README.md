# QODE

A native C++/Qt 6 code editor for Linux: project explorer, tabbed syntax-highlighted editor and an integrated terminal (`Ctrl+J`). See [PRD.md](PRD.md) for the full spec.

## Build

Requires Qt 6 (Core, Gui, Widgets), qmake6 and a C++17 compiler.

```sh
mkdir build && cd build
qmake6 ../QODE.pro
make -j$(nproc)
./qode [project-dir | file ...]
sudo make install        # optional, PREFIX defaults to /usr/local
```

## Language servers (LSP)

QODE is an LSP client; it does not ship any language server. Install the server for your language and QODE finds it on `PATH` (or use **LSP > Set Server Path…**). With no server installed everything else works as before, and the status bar shows a hint when you open a file that has one available.

| Language | Server | Install |
|---|---|---|
| C / C++ | [clangd](https://clangd.llvm.org) | `sudo apt install clangd` · `sudo dnf install clang-tools-extra` · `sudo pacman -S clang` |

clangd needs to know your compiler flags: put a `compile_commands.json` (CMake: `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`; Makefile/qmake projects: `bear -- make`) or a `compile_flags.txt` in the project root or its `build/` folder. Without one clangd guesses default flags (fine for plain files) and may report false errors for project or library headers; in that case use **LSP > Project Compiler Flags…** to give QODE the flags (`-I...`, `-D...`, `-std=...`; there is an "Add Qt 6 Flags" button). They are stored in QODE's settings, not in your project.

**qmake projects** need nothing extra: if the project folder has a `.pro` file, QODE reads it (`QT`, `CONFIG += c++NN`, `INCLUDEPATH`, `DEFINES`, `PKGCONFIG`, `include()`, `SUBDIRS`, and the common platform/Qt-version conditions) and gives clangd the include paths, defines and Qt module headers itself. Saving the `.pro` refreshes them. Flags you add in **Project Compiler Flags…** come after the detected ones.

Currently supported: diagnostics (squiggles, tinted line numbers, error/warning counts in the status bar), hover (rest the mouse on a symbol for its type and documentation) and **Go to Definition** (F12 or Ctrl+click; several candidates are offered in a menu).

**Run** in a qmake application project: the first time you press Run on a C++ source, QODE suggests a command that configures, builds in `build/` and starts the program (`cd {project} && mkdir -p build && cd build && qmake6 ../app.pro && make -j$(nproc) && ./app`); edit it in the Run Configuration dialog if you build differently.

## Layout

```
src/app         QApplication subclass, theme application
src/window      MainWindow (menus, toolbar, splitters, status bar, session)
src/project     ProjectManager, Project, filesystem proxy model, QmakeProject (.pro reader)
src/explorer    ProjectExplorer (QFileSystemModel tree, context menus)
src/editor      Document, CodeEditor, EditorManager, FindBar, syntax highlighting
src/lsp         LspClient (JSON-RPC over stdio), LspManager (servers + document sync), server registry
src/terminal    ShellProcess (pty), TerminalScreen (VT emulator), TerminalView, Terminal
src/dialogs     New project / new file / unsaved-changes dialogs
src/filesystem  FileManager helpers
src/settings    SettingsManager (QSettings), Theme
resources/      qode.qrc, icons, desktop entry
```

Adding a language: write a builder in `src/editor/Language.cpp` and register its extensions in `Registry`.
