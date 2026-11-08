# {{name}}

A Qt 6 Widgets application ({{buildSystem}}).

## Build and run

{{#if !os=win}}
```
./compile.sh     # builds into build/{{nameId}}
./run.sh         # builds what changed, then runs it
```
{{/if}}
{{#if os=win}}
```
compile.bat      # builds into build\{{nameId}}.exe
run.bat          # builds what changed, then runs it with Qt's DLLs on PATH
```

The scripts use the Qt kit this project was created with ({{qmake}}); set QMAKE to another
kit's qmake.exe to switch.
{{/if}}
{{#if buildSystem=qmake}}

The project file is `{{nameId}}.pro`. New `.cpp` / `.h` files must be added to its `SOURCES` / `HEADERS`.
QODE reads it to give the language server (clangd) the Qt include paths.
{{/if}}
{{#if buildSystem=CMake}}

New `.cpp` / `.h` files must be added to `qt_add_executable` in `CMakeLists.txt`. The language server
{{#if !os=win}}
(clangd) reads `build/compile_commands.json`, which appears after the first `./compile.sh`.
{{/if}}
{{#if os=win}}
(clangd) reads `build/compile_commands.json`, which appears after the first `compile.bat`.
{{/if}}
{{/if}}
{{#if ccache}}

## ccache

If [ccache](https://ccache.dev) is installed the build uses it: it stores compiler output and
reuses it when the same code is compiled again.
{{/if}}
{{#if mold}}

## mold

If [mold](https://github.com/rui314/mold) is installed it links your program instead of the
system linker, which is a lot faster.
{{/if}}
