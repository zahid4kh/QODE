# {{name}}

A Qt 6 Widgets application ({{buildSystem}}).

## Build and run

```
./compile.sh     # builds into build/{{nameId}}
./run.sh         # builds what changed, then runs it
```
{{#if buildSystem=qmake}}

The project file is `{{nameId}}.pro`. New `.cpp` / `.h` files must be added to its `SOURCES` / `HEADERS`.
QODE reads it to give the language server (clangd) the Qt include paths.
{{/if}}
{{#if buildSystem=CMake}}

New `.cpp` / `.h` files must be added to `qt_add_executable` in `CMakeLists.txt`. The language server
(clangd) reads `build/compile_commands.json`, which appears after the first `./compile.sh`.
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
