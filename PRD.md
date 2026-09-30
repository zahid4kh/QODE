QODE will be a **native C++/Qt 6 desktop code editor**, not an Electron/web wrapper and not a VS Code fork. I’ll keep this focused on the actual product, UI, behavior, and technical architecture—no roadmap/timeline filler.

# QODE

> Native C++ / Qt 6 code editor for Linux desktop

## 1. Product Overview

**QODE** is a lightweight, native desktop code editor built entirely with:

* **C++**
* **Qt 6**
* **QMake 6**
* Native Qt widgets
* Native filesystem and process APIs

QODE is intended to provide the fundamental workflow of a modern code editor without attempting to reproduce the full complexity of IDEs such as Visual Studio, CLion, or VS Code.

The application should feel like a **real native Linux desktop application**, with no embedded browser, Electron runtime, web frontend, or web-based UI.

### Core workflow

```text
Launch QODE
    ↓
Select / Create Project
    ↓
Project opens
    ↓
Project tree appears
    ↓
Select file
    ↓
File opens in editor
    ↓
Edit / Save
    ↓
Open terminal with Ctrl+J
    ↓
Run project commands
```

---

# 2. Goals

QODE must provide a clean development environment centered around three primary areas:

1. **Project Explorer**
2. **Code Editor**
3. **Integrated Terminal**

The UI should remain simple and unobtrusive.

### Primary goals

* Native Qt desktop application
* Fast startup
* Lightweight UI
* Project-based workflow
* File/project management
* Syntax-highlighted code editing
* Multiple open files
* Integrated terminal
* Resizable editor panels
* Keyboard-driven workflow
* Linux-first desktop experience

### Explicitly out of scope

QODE does **not** initially include:

* AI/chat panel
* AI code generation
* Extensions/plugins
* Debugger
* Git GUI
* Visual designer
* Remote development
* Cloud projects
* Built-in package manager
* Browser/webview
* Electron
* Monaco Editor
* VS Code components

---

# 3. Target Platform

## Primary Platform

**Linux desktop**

Initial development and testing should target modern Linux distributions using Qt 6.

The application should use native Linux filesystem and process behavior wherever practical.

## Future Platforms

The architecture should avoid unnecessary Linux-specific coupling so that Windows and macOS can potentially be supported later.

---

# 4. Technology Stack

| Component           | Technology                                  |
| ------------------- | ------------------------------------------- |
| Language            | C++                                         |
| UI Framework        | Qt 6                                        |
| Build System        | QMake 6                                     |
| GUI                 | Qt Widgets                                  |
| Text Editing        | Qt text/document APIs                       |
| Syntax Highlighting | Custom Qt syntax highlighter                |
| Filesystem          | `QFile`, `QDir`, `QFileInfo`                |
| Processes           | `QProcess`                                  |
| Terminal            | `QProcess` + terminal widget implementation |
| Settings            | `QSettings`                                 |
| Project Metadata    | QODE project file                           |
| Architecture        | C++ classes + Qt signals/slots              |

QODE should use **Qt Widgets rather than Qt Quick/QML** for the initial implementation.

---

# 5. Application Layout

The main QODE window consists of three major regions.

```text
┌───────────────────────────────────────────────────────────────┐
│ QODE                                                         │
├───────────────────────────────────────────────────────────────┤
│ Menu / Toolbar                                               │
├───────────────┬───────────────────────────────────────────────┤
│               │                                               │
│               │                                               │
│   PROJECT     │              CODE EDITOR                      │
│   EXPLORER    │                                               │
│               │                                               │
│               │                                               │
│               │                                               │
│               ├───────────────────────────────────────────────┤
│               │              TERMINAL                         │
│               │                                               │
└───────────────┴───────────────────────────────────────────────┘
```

The interface should use Qt splitters so that:

* Project Explorer width is adjustable.
* Terminal height is adjustable.
* Editor occupies the remaining space.

---

# 6. Main Window

The main application window should be implemented using `QMainWindow`.

### Main regions

```text
QMainWindow
│
├── Menu Bar
├── Toolbar
│
└── Central Widget
    │
    └── Horizontal QSplitter
        │
        ├── Project Explorer
        │
        └── Vertical QSplitter
            │
            ├── Editor
            │
            └── Terminal
```

The splitter arrangement should allow the user to resize the panels interactively.

---

# 7. Menu Bar

QODE should provide a conventional desktop menu.

## File

* New Project
* Open Project
* New File
* Open File
* Save
* Save As
* Save All
* Close File
* Exit

## Edit

* Undo
* Redo
* Cut
* Copy
* Paste
* Select All
* Find
* Replace

## View

* Project Explorer
* Terminal
* Toggle Fullscreen

## Project

* New File
* New Folder
* Open Project Folder
* Close Project

## Help

* About QODE

Menu items should expose standard keyboard shortcuts where appropriate.

---

# 8. Project Management

QODE should operate around projects rather than treating the filesystem as the only organizational concept.

## Create Project

The user should be able to create a new project through:

```text
File → New Project
```

The project creation dialog should request:

* Project name
* Project location

Example:

```text
Project Name:
[ MyProject              ]

Location:
[ /home/user/Projects    ] [Browse]

             [Cancel] [Create]
```

QODE creates the selected directory if necessary.

Example resulting structure:

```text
MyProject/
├── .qode/
└── ...
```

The project root becomes the root displayed in the Project Explorer.

---

# 9. Open Project

The user can open an existing project through:

```text
File → Open Project
```

The file dialog should allow selecting a project directory.

QODE should treat the selected directory as the project root.

A project can therefore be opened even if it was originally created outside QODE.

---

# 10. Project File

QODE should optionally maintain lightweight project metadata.

Example:

```text
.qode/
    project.json
```

The metadata may contain:

```json
{
    "name": "MyProject",
    "version": 1
}
```

The project file must **not** contain the project's source code.

Source files remain normal files on disk.

The `.qode` directory should be optional rather than required for opening arbitrary directories.

---

# 11. Project Explorer

The Project Explorer occupies the left side of the application.

It should display the project filesystem as a tree.

Example:

```text
PROJECT
────────────────────
▾ MyProject
  ▾ src
    main.cpp
    editor.cpp
    editor.h
  ▾ include
    editor.h
  ▸ resources
  CMakeLists.txt
  README.md
```

Although QODE itself uses QMake, projects opened in QODE should **not** be restricted to QMake projects.

The project explorer is fundamentally a filesystem browser.

## Required behavior

* Expand directories
* Collapse directories
* Select files
* Double-click files
* Create files
* Create folders
* Rename files
* Rename folders
* Delete files
* Delete folders
* Refresh project tree
* Reveal filesystem location

The tree should update when filesystem changes are detected.

---

# 12. Creating Files

The user should be able to create a file from the Project Explorer.

Example:

```text
Right-click project/folder
        ↓
New File
        ↓
┌──────────────────────────────┐
│ Create File                  │
│                              │
│ Name: [ main.cpp          ]  │
│                              │
│          [Cancel] [Create]   │
└──────────────────────────────┘
```

QODE creates the file on disk and opens it in the editor.

Supported file extensions should not be artificially restricted.

Examples:

```text
.cpp
.h
.hpp
.c
.cc
.cxx
.py
.js
.ts
.rs
.java
.kt
.json
.xml
.html
.css
.md
.txt
```

---

# 13. Creating Folders

The Project Explorer should provide:

```text
New Folder
```

The user enters a directory name.

The folder is immediately created inside the selected directory.

---

# 14. File Opening

When a user double-clicks a file in the Project Explorer:

1. QODE reads the file.
2. Creates an editor document if necessary.
3. Opens the file in the editor.
4. Determines syntax highlighting from the file extension.
5. Displays the file in an editor tab.

If the file is already open, QODE should activate the existing tab rather than opening a duplicate.

---

# 15. Editor

The editor is the central component of QODE.

It should provide the fundamental functionality expected from a code editor.

## Required features

* Text editing
* Cursor movement
* Text selection
* Copy
* Cut
* Paste
* Undo
* Redo
* Save
* Save As
* Multiple tabs
* Line numbers
* Current-line highlighting
* Syntax highlighting
* Horizontal scrolling
* Vertical scrolling
* Automatic indentation
* Tab handling
* Find
* Replace

---

# 16. Editor Tabs

Multiple files should be open simultaneously.

Example:

```text
┌────────────┬──────────────┬───────────────┐
│ main.cpp × │ editor.cpp × │ editor.h   ×  │
└────────────┴──────────────┴───────────────┘
```

Each tab represents one open document.

A tab should display:

```text
filename
```

and indicate unsaved changes.

Example:

```text
main.cpp *
```

or an equivalent visual indicator.

## Tab behavior

* Click → activate tab
* Middle-click → close tab
* Close button → close tab
* Double-click → optional tab interaction
* Ctrl+Tab → switch tabs
* Ctrl+W → close active tab

If a document contains unsaved changes, QODE must ask whether to:

```text
Save
Discard
Cancel
```

before closing it.

---

# 17. Syntax Highlighting

QODE should provide syntax highlighting based on file extension.

Initial language support should include:

* C
* C++
* Header files
* Python
* JavaScript
* TypeScript
* JSON
* HTML
* CSS
* Markdown
* XML
* Shell scripts

Syntax highlighting should be implemented using Qt's text-document infrastructure.

A dedicated syntax-highlighting architecture should allow additional languages to be added later.

---

# 18. Line Numbers

The editor should display line numbers in a dedicated gutter.

Example:

```text
  1 │ #include <iostream>
  2 │
  3 │ int main()
  4 │ {
  5 │     std::cout << "Hello";
  6 │ }
```

The line-number gutter should scroll vertically together with the document.

---

# 19. Current Line

The line containing the text cursor should receive subtle visual highlighting.

This should make it easy to identify the current editing position without creating a visually heavy interface.

---

# 20. Saving

### Save

```text
Ctrl+S
```

writes the active document to disk.

### Save All

```text
Ctrl+Shift+S
```

or another appropriate shortcut should save all modified documents.

QODE should track document state:

```text
Clean
Modified
Saving
```

The UI should indicate modified documents.

---

# 21. Unsaved Changes

If the user attempts to close:

* a modified tab,
* a project,
* or QODE itself,

while unsaved documents exist, QODE should display a confirmation dialog.

Example:

```text
Save changes to:

• main.cpp
• editor.cpp

[Save All] [Discard] [Cancel]
```

---

# 22. Find and Replace

The editor should provide basic text search.

### Find

```text
Ctrl+F
```

A compact search interface should appear near the editor.

Example:

```text
┌─────────────────────────────────────────┐
│ Find: [ QMainWindow             ]  3/8 │
└─────────────────────────────────────────┘
```

### Replace

```text
Ctrl+H
```

should expose:

* Find
* Replace
* Replace Next
* Replace All

Initial implementation may perform plain-text searching without requiring regex support.

Regex can be added later.

---

# 23. Terminal

QODE should include an integrated terminal at the bottom of the application.

The terminal is hidden or collapsed by default.

### Toggle shortcut

```text
Ctrl+J
```

toggles the terminal.

```text
Editor
────────────────────────────────────────

Terminal
────────────────────────────────────────
$ 
```

Pressing `Ctrl+J` again hides the terminal.

---

# 24. Terminal Behavior

The terminal should operate using the user's native shell.

On Linux, QODE should determine the user's configured shell, for example:

```text
$SHELL
```

Common shells include:

```text
bash
zsh
fish
```

The terminal should start with the project's root directory as its working directory.

Example:

```text
/home/user/Projects/MyProject
$
```

---

# 25. Terminal Features

The initial terminal should support:

* Interactive shell
* Command input
* Command output
* Keyboard input
* Copy
* Paste
* Clear
* Scrollback
* Working directory
* Process termination

Example:

```text
$ ls
CMakeLists.txt
src
README.md

$ g++ src/main.cpp -o app

$ ./app
Hello World
```

QODE should not implement its own shell language.

It should provide an interactive interface to the user's existing shell/process environment.

---

# 26. Terminal Resizing

The terminal/editor boundary must be controlled by a vertical `QSplitter`.

Example:

```text
Editor
Editor
Editor
Editor
────────────── ← draggable splitter
Terminal
Terminal
```

The user can drag the boundary vertically.

The terminal should remember its last height during the current QODE session.

---

# 27. Project Explorer Resizing

The Project Explorer should be separated from the editor by a horizontal `QSplitter`.

Example:

```text
Project       │ Editor
Explorer      │
              │
              │
```

The divider should be draggable.

The user can therefore make the Project Explorer:

* narrow
* medium
* wide

without affecting the editor.

---

# 28. Keyboard Shortcuts

Initial keyboard shortcuts:

| Shortcut           | Action          |
| ------------------ | --------------- |
| `Ctrl+N`           | New File        |
| `Ctrl+O`           | Open File       |
| `Ctrl+Shift+O`     | Open Project    |
| `Ctrl+Shift+N`     | New Project     |
| `Ctrl+S`           | Save            |
| `Ctrl+Shift+S`     | Save As         |
| `Ctrl+Shift+Alt+S` | Save All        |
| `Ctrl+W`           | Close File      |
| `Ctrl+J`           | Toggle Terminal |
| `Ctrl+F`           | Find            |
| `Ctrl+H`           | Replace         |
| `Ctrl+Z`           | Undo            |
| `Ctrl+Shift+Z`     | Redo            |
| `Ctrl+C`           | Copy            |
| `Ctrl+X`           | Cut             |
| `Ctrl+V`           | Paste           |
| `Ctrl+A`           | Select All      |
| `Ctrl+Tab`         | Next Editor Tab |

Shortcuts should use Qt's `QAction` and `QKeySequence` mechanisms.

---

# 29. Context Menus

The Project Explorer should provide context menus.

### File

```text
Open
Open in New Tab
New File
Rename
Delete
Copy Path
Reveal in File Manager
```

### Folder

```text
Open
New File
New Folder
Rename
Delete
Copy Path
Reveal in File Manager
```

### Project Root

```text
New File
New Folder
Refresh
Open in File Manager
Close Project
```

---

# 30. File System Integration

QODE should use Qt filesystem APIs rather than implementing custom filesystem logic.

Primary classes:

```cpp
QFile
QDir
QFileInfo
QFileSystemWatcher
QStandardPaths
```

The project tree should reflect actual files on disk.

External filesystem modifications should be detected where practical.

For example:

```text
External editor creates test.cpp
        ↓
Filesystem watcher detects change
        ↓
QODE refreshes project tree
```

---

# 31. External File Modification

If an opened file changes outside QODE, the editor should detect the change.

Example dialog:

```text
The file "main.cpp" has been modified
outside QODE.

[Reload] [Keep Current]
```

If the current document contains unsaved changes, QODE should avoid silently overwriting them.

---

# 32. Architecture

QODE should use a modular C++ architecture.

Suggested structure:

```text
src/
├── main.cpp
│
├── app/
│   ├── Application.h
│   └── Application.cpp
│
├── window/
│   ├── MainWindow.h
│   └── MainWindow.cpp
│
├── project/
│   ├── Project.h
│   ├── ProjectManager.h
│   ├── ProjectManager.cpp
│   └── ProjectModel.cpp
│
├── explorer/
│   ├── ProjectExplorer.h
│   └── ProjectExplorer.cpp
│
├── editor/
│   ├── Editor.h
│   ├── EditorManager.h
│   ├── EditorTab.h
│   ├── CodeEditor.h
│   └── SyntaxHighlighter.h
│
├── terminal/
│   ├── Terminal.h
│   ├── Terminal.cpp
│   └── ShellProcess.h
│
├── dialogs/
│   ├── NewProjectDialog.h
│   ├── NewFileDialog.h
│   └── UnsavedChangesDialog.h
│
├── filesystem/
│   ├── FileManager.h
│   └── FileManager.cpp
│
└── settings/
    ├── SettingsManager.h
    └── SettingsManager.cpp
```

The exact class structure may evolve, but editor, project, filesystem, and terminal functionality should remain logically separated.

---

# 33. Main Components

## `MainWindow`

Responsible for:

* Main application window
* Menus
* Toolbars
* Global actions
* Splitters
* Connecting major components

---

## `ProjectManager`

Responsible for:

* Opening projects
* Creating projects
* Closing projects
* Current project state
* Project root path
* Project metadata

---

## `ProjectExplorer`

Responsible for:

* Displaying filesystem hierarchy
* File selection
* Directory expansion
* Context menus
* File/folder creation
* Rename/delete operations

The project tree should preferably use Qt's model/view architecture.

Potential classes:

```cpp
QFileSystemModel
QTreeView
```

---

## `EditorManager`

Responsible for:

* Open documents
* Editor tabs
* Active document
* Closing documents
* Save-all operations
* Document lifecycle

---

## `CodeEditor`

Responsible for:

* Text editing
* Cursor
* Selection
* Line numbers
* Syntax highlighting
* Editing behavior
* Search

The editor should be based on Qt's text-editing infrastructure.

---

## `Terminal`

Responsible for:

* Starting the shell
* Sending input
* Receiving process output
* Displaying output
* Terminal scrolling
* Process lifecycle
* Working directory

---

# 34. Document Model

Each opened file should have a document representation containing at minimum:

```text
File path
File name
Text content
Modified state
Encoding
Syntax language
```

The UI editor should operate on the document rather than directly treating every tab as an independent filesystem operation.

This makes features such as:

* Save
* Save As
* Reload
* External modification detection

easier to manage.

---

# 35. Encoding

QODE should initially support UTF-8 as the default encoding.

When opening files:

1. Attempt UTF-8.
2. Handle invalid sequences gracefully.
3. Avoid corrupting files when saving.

The editor should not silently convert a file to another encoding unless explicitly intended.

---

# 36. File Types

QODE should not prevent users from opening unknown file types.

If a file has no recognized syntax definition:

```text
plain text
```

should be used.

Example:

```text
example.xyz
```

opens normally without syntax highlighting.

---

# 37. Application Settings

QODE should use `QSettings` for persistent application preferences.

Initial settings may include:

```text
Window size
Window position
Project Explorer width
Terminal height
Editor font
Editor font size
Theme
Tab size
Use spaces / tabs
Word wrap
```

Settings should be stored using the native Qt settings mechanism.

---

# 38. Session State

QODE may remember the previous session.

Potentially persisted state:

```text
Last opened project
Open files
Active tab
Window size
Panel dimensions
```

However, reopening files should only occur if the files still exist.

---

# 39. Error Handling

QODE should never silently fail filesystem operations.

Examples:

### File cannot be opened

```text
Unable to open file.

Path:
~/project/src/main.cpp

Reason:
Permission denied.

[OK]
```

### File cannot be saved

```text
Unable to save file.

Reason:
Permission denied.

[Retry] [Save As] [Cancel]
```

### Project cannot be opened

The user should receive a clear error explaining why.

---

# 40. Native Desktop Behavior

QODE should behave like a conventional native Qt application.

It should support:

* Native window management
* Native file dialogs
* Native context menus
* Standard keyboard shortcuts
* System clipboard
* Drag and drop
* Desktop window resizing
* Native application menus where appropriate

No browser runtime should be required.

---

# 41. Drag and Drop

The Project Explorer should support dragging files into the editor.

A file dropped onto the editor should open it.

Potential future behavior:

```text
Drag file
    ↓
Editor
    ↓
Open file
```

Dragging files into directories may later support moving files.

---

# 42. Visual Design

QODE should have a minimal developer-oriented interface.

The UI should prioritize:

```text
Code
────────────
Files
────────────
Terminal
```

rather than dashboards, cards, excessive controls, or decorative UI.

### Design characteristics

* Dark-first developer UI
* Compact spacing
* Clear hierarchy
* Subtle borders
* Minimal icons
* High text readability
* Clear active states
* Resizable panels
* No unnecessary UI elements

A light theme may also be provided.

---

# 43. Status Bar

The bottom of the main window may contain a compact status bar.

Example:

```text
main.cpp     C++     UTF-8     LF     Ln 24, Col 8
```

Possible information:

* Current line
* Current column
* File type
* Encoding
* Line ending
* Insert/overwrite state

The terminal remains above the status bar.

---

# 44. Editor Interaction Model

The editor should behave similarly to conventional desktop text editors.

Expected behavior includes:

* Double-click word selection
* Triple-click line selection where supported
* Shift + arrows for selection
* Ctrl + arrows for word navigation
* Home/End navigation
* Ctrl + Home / Ctrl + End
* Standard clipboard behavior
* Standard undo/redo behavior

---

# 45. Project Lifecycle

A project can exist in one of these states:

```text
No Project
    ↓
Project Opening
    ↓
Project Open
    ↓
Project Closing
```

Only one project needs to be open initially.

When a new project is opened while another project contains unsaved files, QODE must ask the user whether to save them before closing the current project.

---

# 46. Multiple Project Support

Initial QODE behavior should use a **single active project**.

The user can:

```text
Close Project
Open another Project
```

Multi-root workspaces are outside the initial scope.

---

# 47. Performance Requirements

QODE should remain responsive during normal editing operations.

The UI should avoid blocking operations on the main thread where unnecessary.

Potentially expensive operations include:

* Loading very large files
* Filesystem traversal
* Searching large directories
* Process output
* External filesystem monitoring

Large project trees should use Qt's model/view architecture rather than constructing thousands of independent widgets.

---

# 48. QMake Project Configuration

The QODE source project should itself be built using QMake 6.

Example high-level project configuration:

```text
QODE.pro
src/
resources/
```

The project should clearly separate:

```text
SOURCES
HEADERS
FORMS
RESOURCES
```

where appropriate.

Qt modules should remain minimal.

Expected initial dependencies:

```text
Qt6 Core
Qt6 Gui
Qt6 Widgets
```

Additional Qt modules should only be introduced when there is a concrete requirement.

---

# 49. Resources

QODE should use Qt resources for application assets.

Example:

```text
resources/
├── icons/
│   ├── folder.svg
│   ├── file.svg
│   ├── close.svg
│   ├── terminal.svg
│   └── ...
└── qode.qrc
```

Icons should remain visually consistent throughout the application.

---

# 50. Security and Process Isolation

QODE executes commands through the user's shell.

The application should clearly treat terminal commands as user-controlled processes.

QODE should not automatically execute arbitrary project commands simply because a project was opened.

For example:

Opening a project:

```text
Project opened
```

must **not** automatically execute:

```text
./build.sh
npm install
make
cmake
```

Commands should only execute through explicit user interaction.

---

# 51. Core MVP Feature Set

The initial QODE release is considered functionally complete when the following workflow works:

```text
Launch QODE
        ↓
Create Project
        ↓
Choose project directory
        ↓
Project Explorer appears
        ↓
Create folder
        ↓
Create main.cpp
        ↓
Open main.cpp
        ↓
Write C++ code
        ↓
Syntax highlighting works
        ↓
Save with Ctrl+S
        ↓
Create another file
        ↓
Open multiple tabs
        ↓
Resize Project Explorer
        ↓
Press Ctrl+J
        ↓
Terminal appears
        ↓
Resize terminal
        ↓
Run compiler/program
        ↓
Close terminal
        ↓
Continue editing
```

This workflow represents the fundamental identity of QODE:

> **A native, lightweight C++ desktop code editor built around projects, files, code, and an integrated terminal.**

---

# 52. Non-Goals for Initial Version

The following should deliberately remain outside the initial product:

```text
AI assistant
Chat
Code completion powered by LLMs
Plugin marketplace
Git GUI
Debugger
Profiler
Remote SSH development
Docker integration
Database tools
Embedded browser
Cloud synchronization
Account system
Telemetry
Project collaboration
```

QODE should first establish a strong native editor foundation before expanding into these areas.
