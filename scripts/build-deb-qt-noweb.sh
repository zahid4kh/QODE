#!/usr/bin/env bash
# Builds qode-bundled-noweb_<version>_<arch>.deb: Qt 6.11 bundled in /opt/qode like scripts/build-deb-qt.sh, but
# without Qt WebEngine (CONFIG+=no_webengine), so there is no web / Expo preview and no Chromium in the package.
#   scripts/build-deb-qt-noweb.sh [--no-build]
exec "$(dirname "$0")/build-deb-qt.sh" --no-webengine "$@"
