#!/usr/bin/env bash
# Builds all three Debian packages and collects them in ./debs (git-ignored):
#   qode                 system Qt 6.4 (dynamic)                scripts/build-deb.sh
#   qode-bundled         Qt 6.11 bundled, with Qt WebEngine     scripts/build-deb-qt.sh
#   qode-bundled-noweb   Qt 6.11 bundled, without Qt WebEngine  scripts/build-deb-qt-noweb.sh
# Each one builds in its own folder, so they do not rebuild each other.  scripts/build-all-debs.sh [--no-build]
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/ui.sh
source scripts/deb-common.sh

scripts/build-deb.sh "$@"
scripts/build-deb-qt.sh "$@"
scripts/build-deb-qt-noweb.sh "$@"

mkdir -p debs
rows=()
for pkg in $QODE_PACKAGES; do
    f="${pkg}_${VERSION}_${ARCH}.deb"
    mv -f "$f" debs/
    rows+=("$pkg|$(du -h "debs/$f" | cut -f1)  debs/$f")
done
summary_box "QODE $VERSION: all packages built" "${rows[@]}"
