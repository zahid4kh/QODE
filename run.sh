#!/usr/bin/env bash
# Run QODE (builds first if needed). Extra args are passed through, e.g. ./run.sh ~/myproject
set -euo pipefail
cd "$(dirname "$0")"

if [[ ! -x build/qode ]]; then
    ./compile.sh
fi
exec ./build/qode "$@"
