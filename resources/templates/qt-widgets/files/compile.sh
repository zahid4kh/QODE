#!/usr/bin/env bash
# Builds {{name}} into build/{{nameId}}.
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build
cd build
{{#if buildSystem=qmake}}
EXTRA=()
{{#if ccache}}

# ccache: reuse earlier compiler output for unchanged code (only if it is installed).
if command -v ccache >/dev/null 2>&1; then
    EXTRA+=("CONFIG+=ccache")
fi
{{/if}}
{{#if mold}}

# mold: a much faster linker (used only if the compiler accepts it).
if command -v mold >/dev/null 2>&1 && echo 'int main(){}' | "${CXX:-g++}" -x c++ - -fuse-ld=mold -o /dev/null 2>/dev/null; then
    EXTRA+=("QMAKE_LFLAGS+=-fuse-ld=mold")
fi
{{/if}}

qmake6 ../{{nameId}}.pro CONFIG+=debug ${EXTRA[@]+"${EXTRA[@]}"}
make -j"$(nproc)"
{{/if}}
{{#if buildSystem=CMake}}
# CMakeLists.txt looks for ccache and mold by itself.
cmake .. -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
cmake --build . -j"$(nproc)"
{{/if}}
