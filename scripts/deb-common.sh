#!/usr/bin/env bash
# Sourced by scripts/build-deb.sh and scripts/build-deb-qt.sh (after scripts/ui.sh): what the three packages share.
#   qode                 the distro's Qt 6 (dynamic linking, Depends on the qt6 packages)       build-deb.sh
#   qode-bundled         Qt 6.11 in /opt/qode, with Qt WebEngine (the web / Expo preview)       build-deb-qt.sh
#   qode-bundled-noweb   Qt 6.11 in /opt/qode, without Qt WebEngine (no preview, much smaller)  build-deb-qt-noweb.sh
# They install the same /usr/bin/qode, so each one conflicts with and replaces the other two.

VERSION="$(sed -n 's/^VERSION *= *//p' QODE.pro | head -n1)"
ARCH="$(dpkg --print-architecture)"
MAINTAINER="Zahid Khalilov <halilzahid@gmail.com>"
HOMEPAGE="https://github.com/zahid4kh/QODE"
QODE_PACKAGES="qode qode-bundled qode-bundled-noweb"

# The other two package names, comma separated.
deb_others() {
    local p out=""
    for p in $QODE_PACKAGES; do
        [ "$p" = "$1" ] || out="${out:+$out, }$p"
    done
    echo "$out"
}

# DEBIAN/{preinst,postinst,prerm,postrm} from packaging/debian/, with the @PACKAGE@, @VERSION@, @FLAVOR@ and @WEB@
# placeholders filled in, so the scripts can say what is being installed.
#   deb_install_scripts <stage> <package> <flavor text> <web 0|1>
deb_install_scripts() {
    local stage="$1" pkg="$2" flavor="$3" web="$4" s
    for s in preinst postinst prerm postrm; do
        sed -e "s|@PACKAGE@|$pkg|g" -e "s|@VERSION@|$VERSION|g" -e "s|@FLAVOR@|$flavor|g" -e "s|@WEB@|$web|g" \
            "packaging/debian/$s" > "$stage/DEBIAN/$s"
        chmod 755 "$stage/DEBIAN/$s"
        ! grep -q '@[A-Z]*@' "$stage/DEBIAN/$s" || die "packaging/debian/$s has a placeholder the build does not fill in"
    done
}

# /usr/share/doc/<package> (README, copyright, changelog) and the man pages.
deb_install_docs() {
    local stage="$1" pkg="$2" page
    local doc="$stage/usr/share/doc/$pkg"
    mkdir -p "$doc" "$stage/usr/share/man/man1"
    [ ! -f README.md ] || install -m 644 README.md "$doc/README.md"
    install -m 644 packaging/copyright "$doc/copyright"
    gzip -9n -c packaging/changelog > "$doc/changelog.gz"
    for page in qode qode-remove-lsp; do
        gzip -9n -c "packaging/$page.1" > "$stage/usr/share/man/man1/$page.1.gz"
    done
}

#   deb_write_control <stage> <package> <depends> <installed size KB> <flavor paragraph> <web 0|1>
deb_write_control() {
    local stage="$1" pkg="$2" depends="$3" size="$4" flavor="$5" web="$6"
    local others provides="" preview
    others="$(deb_others "$pkg")"
    [ "$pkg" = qode ] || provides="Provides: qode"$'\n'
    if [ "$web" = 1 ]; then
        preview=" The live web / Expo preview runs on Qt WebEngine (Chromium)."
    else
        preview=" Built without Qt WebEngine: there is no live web / Expo preview (pages open in the browser)."
    fi
    cat > "$stage/DEBIAN/control" <<CONTROL
Package: $pkg
Version: $VERSION
Section: editors
Priority: optional
Architecture: $ARCH
Depends: $depends
Conflicts: $others
Replaces: $others
${provides}Recommends: git
Suggests: clangd, clang-format
Installed-Size: $size
Maintainer: $MAINTAINER
Homepage: $HOMEPAGE
Description: Native Qt 6 code editor with a built-in coding chat, terminal and LSP client
 QODE is a lightweight code editor: project explorer, tabbed syntax-highlighted
 editor with folding and split views, a chat that reads and edits the project
 (Gemini or Claude), integrated terminal with split panes, Git integration,
 project search, and an LSP client (clangd for C/C++, and JetBrains'
 kotlin-lsp, which QODE can download for you on request).
 .
 $flavor
$preview
 .
 Language servers are never bundled. Removing the package deletes QODE's
 settings, per-project data and downloaded servers for all users; use
 qode-remove-lsp to list or remove language servers.
CONTROL
}

#   deb_summary <deb> <package> <qt text> <web 0|1>
deb_summary() {
    local out="$1" pkg="$2" qt="$3" web="$4" preview="not included"
    [ "$web" = 1 ] && preview="included (Qt WebEngine)"
    summary_box "QODE $VERSION is packaged" \
        "Package|$pkg $VERSION ($ARCH)" \
        "File|$(basename "$out")" \
        "Size|$(du -h "$out" | cut -f1)" \
        "Qt|$qt" \
        "Web preview|$preview" \
        "" \
        "Install|sudo apt install ./$(basename "$out")" \
        "Remove|sudo apt remove $pkg" \
        "Keep data|sudo QODE_KEEP_USER_DATA=1 apt remove $pkg"
}
