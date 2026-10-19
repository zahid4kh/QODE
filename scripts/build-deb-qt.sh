#!/usr/bin/env bash
# Builds a self-contained Debian package that carries its own Qt 6: scripts/build-deb-qt.sh [--no-build]
# (the regular scripts/build-deb.sh keeps depending on the distro's Qt packages)
#   1. compiles QODE (qmake6, PREFIX=/usr) into ./build-deb-qt, unless --no-build
#   2. assembles ./staging_qode_qt:  /opt/qode/{bin,lib,plugins}  (QODE + the Qt libraries and plugins it uses),
#      /usr/bin/qode -> /opt/qode/bin/qode, plus the usual desktop entry, icons, man pages and DEBIAN/ scripts
#   3. runs dpkg-deb  ->  ./qode-bundled_<version>_<arch>.deb
#
# Qt is taken from the machine that builds (qmake6 -query), so the package runs on systems with the same or a newer
# glibc than the build machine (built on Ubuntu 24.04: Ubuntu 24.04+, Debian 13+). Only Qt itself is bundled;
# libicu, libxcb, fontconfig, OpenSSL ... still come from the system and are listed in Depends.
# Needs: qmake6 dpkg-dev fakeroot patchelf (sudo apt install patchelf).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

APP=qode
VERSION="$(sed -n 's/^VERSION *= *//p' QODE.pro | head -n1)"
ARCH="$(dpkg --print-architecture)"
MAINTAINER="Zahid Khalilov <halilzahid@gmail.com>"
HOMEPAGE="https://github.com/zahid4kh/QODE"
STAGE="$ROOT/staging_qode_qt"
OUT="$ROOT/${APP}-bundled_${VERSION}_${ARCH}.deb"
OPT=/opt/qode

# Qt plugin folders QODE can use. Missing ones (e.g. multimedia when Qt Multimedia is not installed) are skipped.
PLUGIN_DIRS="platforms platformthemes platforminputcontexts imageformats iconengines xcbglintegrations
             wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration
             tls networkinformation multimedia"

GREEN='\033[0;32m' BLUE='\033[0;34m' RED='\033[0;31m' NC='\033[0m'
step() { echo -e "${BLUE}==> $*${NC}"; }
die()  { echo -e "${RED}$*${NC}" >&2; exit 1; }

for tool in qmake6 dpkg-deb dpkg-shlibdeps fakeroot patchelf ldd; do
    command -v "$tool" >/dev/null || die "Missing tool: $tool (sudo apt install qt6-base-dev dpkg-dev fakeroot patchelf)"
done
QT_PLUGINS="$(qmake6 -query QT_INSTALL_PLUGINS)"
[ -d "$QT_PLUGINS" ] || die "Qt plugin folder not found: $QT_PLUGINS"

# 1. build
if [ "${1:-}" != "--no-build" ]; then
    step "Building QODE $VERSION (release, PREFIX=/usr)"
    mkdir -p build-deb-qt
    (cd build-deb-qt && qmake6 ../QODE.pro CONFIG+=release PREFIX=/usr && make -j"$(nproc)" >/dev/null)
fi
[ -x build-deb-qt/qode ] || die "build-deb-qt/qode not found; run without --no-build"

# 2. staging tree
step "Assembling $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE$OPT/bin" "$STAGE$OPT/lib" "$STAGE$OPT/plugins"
make -C build-deb-qt install INSTALL_ROOT="$STAGE" >/dev/null   # bin, desktop entry, icons
mv "$STAGE/usr/bin/qode" "$STAGE$OPT/bin/qode"
strip --strip-unneeded "$STAGE$OPT/bin/qode"
ln -s "$OPT/bin/qode" "$STAGE/usr/bin/qode"
install -m 755 packaging/qode-remove-lsp "$STAGE/usr/bin/qode-remove-lsp"
install -m 755 packaging/debian/{preinst,postinst,prerm,postrm} "$STAGE/DEBIAN/"

step "Bundling Qt plugins"
for d in $PLUGIN_DIRS; do
    [ -d "$QT_PLUGINS/$d" ] || continue
    mkdir -p "$STAGE$OPT/plugins/$d"
    cp -L "$QT_PLUGINS/$d"/*.so "$STAGE$OPT/plugins/$d/"
    echo "    $d: $(ls "$STAGE$OPT/plugins/$d" | wc -l) plugin(s)"
done

# Every libQt6* the binary and the plugins load (ldd is recursive). Resolved while nothing is patched yet, so it
# sees the system copies. cp -L stores each under its soname (libQt6Core.so.6).
step "Bundling Qt libraries"
{
    ldd "$STAGE$OPT/bin/qode"
    find "$STAGE$OPT/plugins" -name '*.so' -exec ldd {} +
} 2>/dev/null | awk '$1 ~ /^libQt6/ && $3 ~ /^\// { print $1, $3 }' | sort -u | while read -r soname path; do
    cp -L "$path" "$STAGE$OPT/lib/$soname"
done
ls "$STAGE$OPT/lib" | sed 's/^/    /'
[ -e "$STAGE$OPT/lib/libQt6Core.so.6" ] || die "libQt6Core.so.6 was not found while scanning; is the binary linked against Qt 6?"

# Look only in our own folders, never in the system's Qt: the program finds lib/, libs find each other, plugins find lib/.
# (DT_RPATH on the program is searched for its dependencies' dependencies too.)
step "Setting RPATHs"
patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$STAGE$OPT/bin/qode"
find "$STAGE$OPT/lib" -type f -name '*.so*' -exec patchelf --set-rpath '$ORIGIN' {} +
find "$STAGE$OPT/plugins" -type f -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} +

# qt.conf next to the program: plugins come from /opt/qode/plugins only.
cat > "$STAGE$OPT/bin/qt.conf" <<QTCONF
[Paths]
Prefix = ..
Plugins = plugins
QTCONF

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
find "$STAGE/usr/share" "$STAGE$OPT/lib" "$STAGE$OPT/plugins" "$STAGE$OPT/bin/qt.conf" -type f -exec chmod 644 {} +
chmod 755 "$STAGE$OPT/bin/qode"

# Dependencies of everything we ship, minus Qt (it travels with the package). The bundled libraries have no package
# owner, so dpkg-shlibdeps is told to ignore them; what is left (libc, libstdc++, libicu, xcb, fontconfig, OpenSSL,
# ffmpeg for the multimedia plugin ...) is what the system has to provide.
step "Computing dependencies"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/debian"
printf 'Source: qode\n\nPackage: qode\nArchitecture: any\n' > "$TMP/debian/control"
ELFS=(-e"$STAGE$OPT/bin/qode")
while IFS= read -r f; do ELFS+=(-e"$f"); done < <(find "$STAGE$OPT/lib" "$STAGE$OPT/plugins" -type f -name '*.so*')
SHLIBS="$(cd "$TMP" && dpkg-shlibdeps -O --ignore-missing-info -l"$STAGE$OPT/lib" "${ELFS[@]}" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
if grep -qi 'libqt6' <<<"$SHLIBS"; then
    die "Depends still mentions a system Qt package, so something links outside the bundle: $SHLIBS"
fi
DEPENDS="${SHLIBS:+$SHLIBS, }tar (>= 1.29)"
echo "    Depends: $DEPENDS"

SIZE_KB="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: $APP-bundled
Version: $VERSION
Section: editors
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Conflicts: $APP
Replaces: $APP
Provides: $APP
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
 This build carries its own copy of Qt 6 in /opt/qode (no Qt packages needed).
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
echo "Check:     ldd $STAGE$OPT/bin/qode | grep -i qt    (every Qt line must point into $STAGE$OPT/lib)"
echo "Install:   sudo apt install ./$(basename "$OUT")"
echo "Remove:    sudo apt remove qode-bundled     (also wipes /opt/qode and QODE's per-user data; QODE_KEEP_USER_DATA=1 keeps the user data)"
