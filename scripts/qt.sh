#!/usr/bin/env bash
# Sourced by the build scripts: picks the Qt to build with and sets QMAKE to its qmake.
#   1. $QODE_QMAKE, when set (e.g. QODE_QMAKE=/usr/bin/qmake6 for the distro's Qt)
#   2. the newest Qt installed under ~/Qt (online installer / aqtinstall layout: ~/Qt/<version>/gcc_64/bin/qmake6)
#   3. qmake6 from PATH
# A Qt from ~/Qt is never installed system wide; the binary finds its libraries through the RUNPATH qmake adds.

qt_pick_qmake() {
    if [ -n "${QODE_QMAKE:-}" ]; then
        echo "$QODE_QMAKE"
        return
    fi
    local newest
    newest="$(ls -d "$HOME"/Qt/[0-9]*/gcc_64/bin/qmake6 2>/dev/null | sort -V | tail -n1)"
    if [ -n "$newest" ]; then
        echo "$newest"
        return
    fi
    command -v qmake6 || true
}

QMAKE="$(qt_pick_qmake)"
[ -n "$QMAKE" ] && [ -x "$QMAKE" ] || { echo "No qmake6 found (install Qt 6 under ~/Qt or set QODE_QMAKE)" >&2; exit 1; }
QT_VERSION_USED="$("$QMAKE" -query QT_VERSION)"

# A build folder configured by another Qt has objects and moc files of that Qt: start it over.
qt_prepare_build_dir() {
    local dir="$1"
    mkdir -p "$dir"
    if [ -f "$dir/Makefile" ] && ! grep -qxF "QMAKE         = $QMAKE" "$dir/Makefile"; then
        echo "$dir was configured with another Qt; cleaning it for Qt $QT_VERSION_USED"
        find "$dir" -mindepth 1 -delete
    fi
}
