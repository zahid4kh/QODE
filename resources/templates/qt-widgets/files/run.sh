#!/usr/bin/env bash
# Builds what changed, then runs {{name}}.   Usage: ./run.sh [arguments for the program]
set -euo pipefail
cd "$(dirname "$0")"

./compile.sh
exec ./build/{{nameId}} "$@"
