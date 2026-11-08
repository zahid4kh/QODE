#!/usr/bin/env bash
# Builds the Debian package that uses the distro's Qt (dynamic linking): scripts/build-deb.sh [--no-build]
#   1. compiles QODE (the distro's qmake6, PREFIX=/usr) into ./build-deb, unless --no-build
#   2. assembles the staging tree ./staging_qode (DEBIAN/ scripts come from packaging/debian/)
#   3. runs dpkg-deb  ->  ./qode_<version>_<arch>.deb
# The staging folder and the .deb are generated files and are git-ignored.
# The package depends on the distro's qt6 packages, so it is always built against the distro's Qt (/usr/bin/qmake6),
# never a newer Qt from ~/Qt: that one is not in apt, and a package linked to it would not run where it is installed.
# To ship Qt 6.11 inside the package, use scripts/build-deb-qt.sh (with the web preview) or
# scripts/build-deb-qt-noweb.sh (without it, smaller).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
source scripts/ui.sh
source scripts/deb-common.sh

APP=qode
STAGE="$ROOT/staging_qode"
OUT="$ROOT/${APP}_${VERSION}_${ARCH}.deb"
BUILD_FIRST=1
[ "${1:-}" != "--no-build" ] || BUILD_FIRST=0

QODE_QMAKE=/usr/bin/qmake6
for tool in "$QODE_QMAKE" dpkg-deb dpkg-shlibdeps fakeroot; do
    command -v "$tool" >/dev/null || die "Missing tool: $tool (sudo apt install qt6-base-dev dpkg-dev fakeroot)"
done
source scripts/qt.sh

UI_STEPS=$((3 + BUILD_FIRST))
banner "QODE $VERSION  ·  $APP" "Debian package linked against the system Qt $QT_VERSION_USED (needs the distro's qt6 packages)"

# 1. build
if [ "$BUILD_FIRST" = 1 ]; then
    step "Building QODE $VERSION (release, PREFIX=/usr, Qt $QT_VERSION_USED)"
    qt_prepare_build_dir build-deb
    (cd build-deb && "$QMAKE" ../QODE.pro CONFIG+=release PREFIX=/usr >/dev/null && make -j"$(nproc)" >/dev/null)
    ok "build-deb/qode"
fi
[ -x build-deb/qode ] || die "build-deb/qode not found; run without --no-build"
WEB=0
grep -q QODE_HAS_WEBENGINE build-deb/Makefile && WEB=1

# 2. staging tree
step "Assembling $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
make -C build-deb install INSTALL_ROOT="$STAGE" >/dev/null   # bin, desktop entry, icons
strip --strip-unneeded "$STAGE/usr/bin/qode"
install -m 755 packaging/qode-remove-lsp "$STAGE/usr/bin/qode-remove-lsp"
deb_install_scripts "$STAGE" "$APP" "system Qt $QT_VERSION_USED" "$WEB"
deb_install_docs "$STAGE" "$APP"
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE/usr/share" -type f -exec chmod 644 {} +
ok "staged ($(du -sh --exclude=DEBIAN "$STAGE" | cut -f1))"

# Dependencies from the libraries the binary really links against (+ tools QODE shells out to).
step "Computing dependencies"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/debian"
printf 'Source: qode\n\nPackage: qode\nArchitecture: any\n' > "$TMP/debian/control"
SHLIBS="$(cd "$TMP" && dpkg-shlibdeps -O -e"$STAGE/usr/bin/qode" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
DEPENDS="${SHLIBS:+$SHLIBS, }tar (>= 1.29)"
info "Depends: $DEPENDS"

SIZE_KB="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"
deb_write_control "$STAGE" "$APP" "$DEPENDS" "$SIZE_KB" "This build uses the distribution's Qt 6 packages ($QT_VERSION_USED)." "$WEB"

# 3. package
step "Building $(basename "$OUT")"
rm -f "$OUT"
fakeroot dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
ok "dpkg-deb"

deb_summary "$OUT" "$APP" "system Qt $QT_VERSION_USED (dynamic)" "$WEB"
