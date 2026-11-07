# {{name}}

A C++{{std}} console program built with {{compiler}}.

## Build and run

{{#if !os=win}}
```
./compile.sh     # builds into build/{{nameId}}
./run.sh         # builds what changed, then runs it
```

`compile.sh` builds every `.cpp` under `src/`, so adding a file needs no edits.
{{/if}}
{{#if os=win}}
```
compile.bat      # builds into build\{{nameId}}.exe
run.bat          # builds, then runs it
```

`compile.bat` builds every `.cpp` under `src\`, so adding a file needs no edits.
{{/if}}
Headers go in `include/`.
{{#if buildSystem=CMake}}

The scripts call CMake (see `CMakeLists.txt`). `build/compile_commands.json` is what the
language server (clangd) reads; `compile_flags.txt` covers the time before the first build.
{{/if}}
{{#if buildSystem=Scripts}}

{{#if !os=win}}
`compile_flags.txt` tells the language server (clangd) which flags you compile with. Keep it
in step when you change the flags in `compile.sh`.
{{/if}}
{{#if os=win}}
`compile_flags.txt` tells the language server (clangd) which flags you compile with. Keep it
in step when you change the flags in `compile.bat`.
{{/if}}
{{/if}}
{{#if tests}}

## Tests

{{#if !os=win}}
`./compile.sh test` builds and runs `tests/test_main.cpp`.
{{/if}}
{{#if os=win}}
`compile.bat test` builds and runs `tests\test_main.cpp`.
{{/if}}
{{/if}}
{{#if ccache}}

## ccache

If [ccache](https://ccache.dev) is installed the scripts use it: it stores compiler output and
reuses it when the same code is compiled again.
{{/if}}
{{#if mold}}

## mold

If [mold](https://github.com/rui314/mold) is installed it links your program instead of the
system linker, which is a lot faster for big programs.
{{/if}}
