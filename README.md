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

## Layout

```
src/app         QApplication subclass, theme application
src/window      MainWindow (menus, toolbar, splitters, status bar, session)
src/project     ProjectManager, Project, filesystem proxy model
src/explorer    ProjectExplorer (QFileSystemModel tree, context menus)
src/editor      Document, CodeEditor, EditorManager, FindBar, syntax highlighting
src/terminal    ShellProcess (pty), TerminalScreen (VT emulator), TerminalView, Terminal
src/dialogs     New project / new file / unsaved-changes dialogs
src/filesystem  FileManager helpers
src/settings    SettingsManager (QSettings), Theme
resources/      qode.qrc, icons, desktop entry
```

Adding a language: write a builder in `src/editor/Language.cpp` and register its extensions in `Registry`.
