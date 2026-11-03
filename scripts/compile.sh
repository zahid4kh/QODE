#!/usr/bin/env bash
# Configure (qmake) and build QODE into ./build, with the Qt scripts/qt.sh picks (newest under ~/Qt, else qmake6).
# Extra arguments go to qmake, e.g. scripts/compile.sh CONFIG+=no_webengine
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/ui.sh
source scripts/qt.sh

VERSION="$(sed -n 's/^VERSION *= *//p' QODE.pro | head -n1)"
banner "QODE $VERSION" "release build into build/ with Qt $QT_VERSION_USED"
kv "qmake" "$QMAKE"

qt_prepare_build_dir build
cd build
"$QMAKE" ../QODE.pro CONFIG+=release "$@"
make -j"$(nproc)"
if grep -q QODE_HAS_WEBENGINE Makefile; then web="yes (Qt WebEngine)"; else web="no"; fi
echo
ok "Built $(pwd)/qode"
kv "Qt" "$QT_VERSION_USED"
kv "Web preview" "$web"
