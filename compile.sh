#!/usr/bin/env bash
# Configure (qmake6) and build QODE into ./build
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build
cd build
qmake6 ../QODE.pro CONFIG+=release
make -j"$(nproc)"
echo "Built: $(pwd)/qode"
