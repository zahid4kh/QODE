#!/usr/bin/env bash
# Builds {{name}} into build/{{nameId}}.   Usage: ./compile.sh [test]
set -euo pipefail
cd "$(dirname "$0")"

{{#if buildSystem=CMake}}
export CXX="${CXX:-{{compiler}}}"
cmake -S . -B build -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
cmake --build build -j"$(nproc)"
{{#if tests}}
if [[ "${1:-}" == "test" ]]; then
    ctest --test-dir build --output-on-failure
fi
{{/if}}
{{/if}}
{{#if buildSystem=Scripts}}
CXX="${CXX:-{{compiler}}}"
CXXFLAGS=(-std=c++{{std}} -Wall -Wextra -Wpedantic -g -Iinclude -MMD -MP)
LDFLAGS=()
COMPILE=("$CXX")
{{#if ccache}}

# ccache: reuse earlier compiler output for unchanged code.
if command -v ccache >/dev/null 2>&1; then
    COMPILE=(ccache "$CXX")
fi
{{/if}}
{{#if mold}}

# mold: a much faster linker (used only if the compiler accepts it).
if command -v mold >/dev/null 2>&1 && echo 'int main(){}' | "$CXX" -x c++ - -fuse-ld=mold -o /dev/null 2>/dev/null; then
    LDFLAGS+=(-fuse-ld=mold)
fi
{{/if}}

# An object file is stale when it is missing or it, or any header it includes, changed.
stale() {
    local obj="$1" dep
    [[ -f "$obj" && -f "${obj%.o}.d" ]] || return 0
    for dep in $(sed -e 's/^[^:]*://' -e 's/\\$//' "${obj%.o}.d"); do
        [[ -e "$dep" && ! "$dep" -nt "$obj" ]] || return 0
    done
    return 1
}

# build <output> <sources...>: compiles each source to build/obj/ (only when stale) and links them.
build() {
    local out="$1" src obj objs=() dirty=0
    shift
    for src in "$@"; do
        obj="build/obj/${src%.cpp}.o"
        mkdir -p "$(dirname "$obj")"
        if stale "$obj"; then
            echo "  CXX  $src"
            "${COMPILE[@]}" "${CXXFLAGS[@]}" -c "$src" -o "$obj"
            dirty=1
        fi
        objs+=("$obj")
    done
    [[ $dirty == 1 || ! -e "$out" ]] || return 0
    echo "  LINK $out"
    "$CXX" "${objs[@]}" "${LDFLAGS[@]}" -o "$out"
}

mkdir -p build
mapfile -t sources < <(find src -name '*.cpp' | sort)
build "build/{{nameId}}" "${sources[@]}"
{{#if tests}}

if [[ "${1:-}" == "test" ]]; then
    mapfile -t library < <(find src -name '*.cpp' ! -name main.cpp | sort)
    build build/tests "${library[@]}" tests/test_main.cpp
    ./build/tests
fi
{{/if}}
{{/if}}
