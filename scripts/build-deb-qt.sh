#!/usr/bin/env bash
# Builds a self-contained Debian package that carries its own Qt 6 (6.11 from ~/Qt):
#   scripts/build-deb-qt.sh [--no-build] [--no-webengine]
# (scripts/build-deb.sh keeps depending on the distro's Qt packages; scripts/build-deb-qt-noweb.sh is this script with
# --no-webengine)
#   1. compiles QODE (PREFIX=/usr) into ./build-deb-qt (./build-deb-qt-noweb) with the Qt scripts/qt.sh picks (newest
#      under ~/Qt, else qmake6; QODE_QMAKE=... to choose), unless --no-build (then the Qt that configured it is used)
#   2. assembles ./staging_qode_qt (./staging_qode_qt_noweb):  /opt/qode/{bin,lib,plugins}  (QODE + the Qt libraries
#      and plugins it uses), /opt/qode/{libexec,resources,translations}  (Qt WebEngine's helper process, its .pak
#      files and locales; not with --no-webengine), /usr/bin/qode -> /opt/qode/bin/qode, plus the usual desktop
#      entry, icons, man pages and DEBIAN/ scripts
#   3. runs dpkg-deb  ->  ./qode-bundled_<version>_<arch>.deb  (./qode-bundled-noweb_<version>_<arch>.deb)
#
# --no-webengine builds QODE with CONFIG+=no_webengine even though this Qt has WebEngine: no web / Expo preview, and
# none of Chromium (QtWebEngineProcess, its resources, libQt6WebEngine*, Qt Quick under it) goes into the package.
#
# The package runs on systems with the same or a newer glibc than the build machine (and than the one the Qt build
# needs). Qt is bundled, with whatever else sits in Qt's own lib folder (an installer Qt ships libicu and FFmpeg
# there); libxcb, fontconfig, OpenSSL, NSS ... still come from the system and are listed in Depends.
# Needs: qmake6 dpkg-dev fakeroot patchelf (sudo apt install patchelf).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
source scripts/ui.sh
source scripts/deb-common.sh

BUILD_FIRST=1 WEB=1
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD_FIRST=0 ;;
        --no-webengine) WEB=0 ;;
        -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown option: $arg (see --help)" ;;
    esac
done

if [ "$WEB" = 1 ]; then
    APP=qode-bundled BUILD=build-deb-qt STAGE="$ROOT/staging_qode_qt" QMAKE_EXTRA=()
else
    APP=qode-bundled-noweb BUILD=build-deb-qt-noweb STAGE="$ROOT/staging_qode_qt_noweb" QMAKE_EXTRA=(CONFIG+=no_webengine)
fi
OUT="$ROOT/${APP}_${VERSION}_${ARCH}.deb"
OPT=/opt/qode

# Qt plugin folders QODE can use. Missing ones (e.g. multimedia when Qt Multimedia is not installed) are skipped.
PLUGIN_DIRS="platforms platformthemes platforminputcontexts imageformats iconengines xcbglintegrations
             wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration
             tls networkinformation multimedia"

if [ "$BUILD_FIRST" = 0 ] && [ -z "${QODE_QMAKE:-}" ] && [ -f "$BUILD/Makefile" ]; then
    QODE_QMAKE="$(sed -n 's/^QMAKE *= *//p' "$BUILD/Makefile" | head -n1)"
fi
source scripts/qt.sh
for tool in dpkg-deb dpkg-shlibdeps fakeroot patchelf ldd; do
    command -v "$tool" >/dev/null || die "Missing tool: $tool (sudo apt install dpkg-dev fakeroot patchelf)"
done
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
QT_LIBS="$(realpath "$("$QMAKE" -query QT_INSTALL_LIBS)")"
QT_LIBEXEC="$("$QMAKE" -query QT_INSTALL_LIBEXECS)"
QT_DATA="$("$QMAKE" -query QT_INSTALL_DATA)"
QT_TRANSLATIONS="$("$QMAKE" -query QT_INSTALL_TRANSLATIONS)"
[ -d "$QT_PLUGINS" ] || die "Qt plugin folder not found: $QT_PLUGINS"
if [ "$WEB" = 1 ] && [ ! -x "$QT_LIBEXEC/QtWebEngineProcess" ]; then
    die "Qt $QT_VERSION_USED ($QMAKE) has no Qt WebEngine; install it, or build without the preview: scripts/build-deb-qt-noweb.sh"
fi

UI_STEPS=$((7 + BUILD_FIRST))
if [ "$WEB" = 1 ]; then
    banner "QODE $VERSION  ·  $APP" "Qt $QT_VERSION_USED bundled in $OPT, with Qt WebEngine (web / Expo preview)"
else
    banner "QODE $VERSION  ·  $APP" "Qt $QT_VERSION_USED bundled in $OPT, without Qt WebEngine (no preview, smaller)"
fi
kv "qmake" "$QMAKE"

# 1. build
if [ "$BUILD_FIRST" = 1 ]; then
    step "Building QODE $VERSION (release, PREFIX=/usr, Qt $QT_VERSION_USED${QMAKE_EXTRA[*]:+, ${QMAKE_EXTRA[*]}})"
    qt_prepare_build_dir "$BUILD"
    (cd "$BUILD" && "$QMAKE" ../QODE.pro CONFIG+=release PREFIX=/usr "${QMAKE_EXTRA[@]}" >/dev/null && make -j"$(nproc)" >/dev/null)
    ok "$BUILD/qode"
fi
[ -x "$BUILD/qode" ] || die "$BUILD/qode not found; run without --no-build"
if ldd "$BUILD/qode" | grep -q libQt6WebEngineCore; then
    [ "$WEB" = 1 ] || die "$BUILD/qode links Qt WebEngine although it was asked to build without it; delete $BUILD and run again"
else
    [ "$WEB" = 0 ] || die "$BUILD/qode was built without Qt WebEngine; delete $BUILD and run again"
fi

# 2. staging tree
step "Assembling $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE$OPT/bin" "$STAGE$OPT/lib" "$STAGE$OPT/plugins"
make -C "$BUILD" install INSTALL_ROOT="$STAGE" >/dev/null   # bin, desktop entry, icons
mv "$STAGE/usr/bin/qode" "$STAGE$OPT/bin/qode"
strip --strip-unneeded "$STAGE$OPT/bin/qode"
ln -s "$OPT/bin/qode" "$STAGE/usr/bin/qode"
install -m 755 packaging/qode-remove-lsp "$STAGE/usr/bin/qode-remove-lsp"
if [ "$WEB" = 1 ]; then
    deb_install_scripts "$STAGE" "$APP" "Qt $QT_VERSION_USED bundled in $OPT" "$WEB"
else
    deb_install_scripts "$STAGE" "$APP" "Qt $QT_VERSION_USED bundled in $OPT (without Qt WebEngine)" "$WEB"
fi

step "Bundling Qt plugins"
for d in $PLUGIN_DIRS; do
    [ -d "$QT_PLUGINS/$d" ] || continue
    mkdir -p "$STAGE$OPT/plugins/$d"
    cp -L "$QT_PLUGINS/$d"/*.so "$STAGE$OPT/plugins/$d/"
    info "$(printf '%-38s %s plugin(s)' "$d" "$(ls "$STAGE$OPT/plugins/$d" | wc -l)")"
done

# Qt WebEngine (the web preview): Chromium runs in QtWebEngineProcess, which needs the .pak resources, icudtl.dat
# and the locales next to it.
if [ "$WEB" = 1 ]; then
    step "Bundling Qt WebEngine"
    mkdir -p "$STAGE$OPT/libexec" "$STAGE$OPT/translations"
    cp -L "$QT_LIBEXEC/QtWebEngineProcess" "$STAGE$OPT/libexec/"
    cp -rL "$QT_DATA/resources" "$STAGE$OPT/resources"
    [ ! -d "$QT_TRANSLATIONS/qtwebengine_locales" ] || cp -rL "$QT_TRANSLATIONS/qtwebengine_locales" "$STAGE$OPT/translations/"
    ok "QtWebEngineProcess, resources ($(du -sh "$STAGE$OPT/resources" | cut -f1)), locales"
else
    step "Skipping Qt WebEngine (--no-webengine)"
    info "no QtWebEngineProcess, no Chromium resources, no web / Expo preview"
fi

# Every libQt6* the binary, the plugins and the WebEngine helper load (ldd is recursive), and anything else that
# resolves into Qt's own lib folder (libicu, FFmpeg). ldd runs on Qt's own plugins and helper, not the staged copies:
# those keep a RUNPATH relative to Qt's folder until patched, which points into the empty staging lib/ for a plugin.
# cp -L stores each under its soname (libQt6Core.so.6).
step "Bundling Qt libraries"
{
    ldd "$STAGE$OPT/bin/qode"
    for d in $PLUGIN_DIRS; do
        [ ! -d "$QT_PLUGINS/$d" ] || find "$QT_PLUGINS/$d" -maxdepth 1 -name '*.so' -exec ldd {} +
    done
    [ ! -e "$STAGE$OPT/libexec/QtWebEngineProcess" ] || ldd "$QT_LIBEXEC/QtWebEngineProcess"
} 2>/dev/null | awk '$3 ~ /^\// { print $1, $3 }' | sort -u | while read -r soname path; do
    if [[ "$soname" == libQt6* ]] || [[ "$(realpath "$path")" == "$QT_LIBS"/* ]]; then
        cp -L "$path" "$STAGE$OPT/lib/$soname"
    fi
done
[ -e "$STAGE$OPT/lib/libQt6Core.so.6" ] || die "libQt6Core.so.6 was not found while scanning; is the binary linked against Qt 6?"
if [ "$WEB" = 0 ] && ls "$STAGE$OPT/lib" | grep -q '^libQt6WebEngine'; then
    die "a Qt WebEngine library was bundled with --no-webengine: $(ls "$STAGE$OPT/lib" | grep '^libQt6WebEngine' | tr '\n' ' ')"
fi
ls "$STAGE$OPT/lib" | sed 's/^/    /'
ok "$(ls "$STAGE$OPT/lib" | wc -l) libraries, $(du -sh "$STAGE$OPT/lib" | cut -f1)"

# Look only in our own folders, never in the system's Qt: the program finds lib/, libs find each other, plugins find lib/.
# (DT_RPATH on the program is searched for its dependencies' dependencies too.)
step "Setting RPATHs"
patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$STAGE$OPT/bin/qode"
find "$STAGE$OPT/lib" -type f -name '*.so*' -exec patchelf --set-rpath '$ORIGIN' {} +
find "$STAGE$OPT/plugins" -type f -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} +
[ ! -e "$STAGE$OPT/libexec/QtWebEngineProcess" ] || patchelf --set-rpath '$ORIGIN/../lib' "$STAGE$OPT/libexec/QtWebEngineProcess"

# qt.conf next to the program: plugins, the WebEngine helper and its data come from /opt/qode only.
cat > "$STAGE$OPT/bin/qt.conf" <<QTCONF
[Paths]
Prefix = ..
Plugins = plugins
LibraryExecutables = libexec
Data = .
Translations = translations
QTCONF
ok "qt.conf"

deb_install_docs "$STAGE" "$APP"
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE/usr/share" "$STAGE$OPT/lib" "$STAGE$OPT/plugins" "$STAGE$OPT/bin/qt.conf" -type f -exec chmod 644 {} +
[ ! -d "$STAGE$OPT/resources" ] || find "$STAGE$OPT/resources" "$STAGE$OPT/translations" -type f -exec chmod 644 {} +
chmod 755 "$STAGE$OPT/bin/qode"
[ ! -e "$STAGE$OPT/libexec/QtWebEngineProcess" ] || chmod 755 "$STAGE$OPT/libexec/QtWebEngineProcess"

# Dependencies of everything we ship, minus Qt (it travels with the package). The bundled libraries have no package
# owner, so dpkg-shlibdeps is told to ignore them; what is left (libc, libstdc++, xcb, fontconfig, OpenSSL, NSS,
# ffmpeg for the multimedia plugin ...) is what the system has to provide.
step "Computing dependencies"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/debian"
printf 'Source: qode\n\nPackage: qode\nArchitecture: any\n' > "$TMP/debian/control"
ELFS=(-e"$STAGE$OPT/bin/qode")
[ ! -e "$STAGE$OPT/libexec/QtWebEngineProcess" ] || ELFS+=(-e"$STAGE$OPT/libexec/QtWebEngineProcess")
while IFS= read -r f; do ELFS+=(-e"$f"); done < <(find "$STAGE$OPT/lib" "$STAGE$OPT/plugins" -type f -name '*.so*')
SHLIBS="$(cd "$TMP" && dpkg-shlibdeps -O --ignore-missing-info -l"$STAGE$OPT/lib" "${ELFS[@]}" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
if grep -qi 'libqt6' <<<"$SHLIBS"; then
    die "Depends still mentions a system Qt package, so something links outside the bundle: $SHLIBS"
fi
DEPENDS="${SHLIBS:+$SHLIBS, }tar (>= 1.29)"
info "Depends: $DEPENDS"

SIZE_KB="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"
deb_write_control "$STAGE" "$APP" "$DEPENDS" "$SIZE_KB" "This build carries its own copy of Qt $QT_VERSION_USED in $OPT (no Qt packages needed)." "$WEB"

# 3. package
step "Building $(basename "$OUT")"
rm -f "$OUT"
fakeroot dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
ok "dpkg-deb"
info "check: ldd $STAGE$OPT/bin/qode | grep -i qt   (every Qt line must point into $STAGE$OPT/lib)"

deb_summary "$OUT" "$APP" "$QT_VERSION_USED, bundled in $OPT" "$WEB"
