#!/usr/bin/env bash
# Builds the Debian package: ./build-deb.sh [--no-build]
#   1. compiles QODE (qmake6, PREFIX=/usr) into ./build-deb, unless --no-build
#   2. assembles the staging tree ./staging_qode (DEBIAN/ scripts come from packaging/debian/)
#   3. runs dpkg-deb  ->  ./qode_<version>_<arch>.deb
# The staging folder and the .deb are generated files and are git-ignored.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"

APP=qode
VERSION="$(sed -n 's/^VERSION *= *//p' QODE.pro | head -n1)"
ARCH="$(dpkg --print-architecture)"
MAINTAINER="Zahid Khalilov <halilzahid@gmail.com>"
HOMEPAGE="https://github.com/zahid4kh/QODE"
STAGE="$ROOT/staging_qode"
OUT="$ROOT/${APP}_${VERSION}_${ARCH}.deb"

GREEN='\033[0;32m' BLUE='\033[0;34m' RED='\033[0;31m' NC='\033[0m'
step() { echo -e "${BLUE}==> $*${NC}"; }

for tool in qmake6 dpkg-deb dpkg-shlibdeps fakeroot; do
    command -v "$tool" >/dev/null || { echo -e "${RED}Missing tool: $tool (sudo apt install qt6-base-dev dpkg-dev fakeroot)${NC}" >&2; exit 1; }
done

# 1. build
if [ "${1:-}" != "--no-build" ]; then
    step "Building QODE $VERSION (release, PREFIX=/usr)"
    mkdir -p build-deb
    (cd build-deb && qmake6 ../QODE.pro CONFIG+=release PREFIX=/usr && make -j"$(nproc)" >/dev/null)
fi
[ -x build-deb/qode ] || { echo -e "${RED}build-deb/qode not found; run without --no-build${NC}" >&2; exit 1; }

# 2. staging tree
step "Assembling $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
make -C build-deb install INSTALL_ROOT="$STAGE" >/dev/null   # bin, desktop entry, icons
strip --strip-unneeded "$STAGE/usr/bin/qode"
install -m 755 packaging/qode-remove-lsp "$STAGE/usr/bin/qode-remove-lsp"
install -m 755 packaging/debian/{preinst,postinst,prerm,postrm} "$STAGE/DEBIAN/"

DOC="$STAGE/usr/share/doc/$APP"
mkdir -p "$DOC"
install -m 644 README.md "$DOC/README.md"
install -m 644 packaging/copyright "$DOC/copyright"
gzip -9n -c packaging/changelog > "$DOC/changelog.gz"
mkdir -p "$STAGE/usr/share/man/man1"
for page in qode qode-remove-lsp; do
    gzip -9n -c "packaging/$page.1" > "$STAGE/usr/share/man/man1/$page.1.gz"
done
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE/usr/share" -type f -exec chmod 644 {} +

# Dependencies from the libraries the binary really links against (+ tools QODE shells out to).
step "Computing dependencies"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/debian"
printf 'Source: qode\n\nPackage: qode\nArchitecture: any\n' > "$TMP/debian/control"
SHLIBS="$(cd "$TMP" && dpkg-shlibdeps -O -e"$STAGE/usr/bin/qode" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
DEPENDS="${SHLIBS:+$SHLIBS, }tar (>= 1.29)"
echo "    Depends: $DEPENDS"

SIZE_KB="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: $APP
Version: $VERSION
Section: editors
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Recommends: git
Suggests: clangd, clang-format
Installed-Size: $SIZE_KB
Maintainer: $MAINTAINER
Homepage: $HOMEPAGE
Description: Native Qt 6 code editor with built-in terminal and language server client
 QODE is a lightweight code editor: project explorer, tabbed syntax-highlighted
 editor with folding and split views, integrated terminal with split panes,
 Git integration, project search, and an LSP client (clangd for C/C++, and
 JetBrains' kotlin-lsp, which QODE can download for you on request).
 .
 Language servers are never bundled. Removing the package deletes QODE's
 settings, per-project data and downloaded servers for all users; use
 qode-remove-lsp to list or remove language servers.
CONTROL

# 3. package
step "Building $OUT"
rm -f "$OUT"
fakeroot dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
echo -e "${GREEN}Done: $OUT ($(du -h "$OUT" | cut -f1))${NC}"
echo "Install:   sudo apt install ./$(basename "$OUT")"
echo "Remove:    sudo apt remove qode        (also wipes QODE's per-user data; QODE_KEEP_USER_DATA=1 keeps it)"
