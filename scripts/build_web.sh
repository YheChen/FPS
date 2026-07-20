#!/usr/bin/env bash
# Builds the WebAssembly client and (optionally) serves it locally.
# Requires the Emscripten SDK; set EMSDK or install to ~/emsdk (see
# docs/build.md).
set -euo pipefail

cd "$(dirname "$0")/.."

EMSDK_DIR="${EMSDK:-$HOME/emsdk}"
if [ ! -f "${EMSDK_DIR}/emsdk_env.sh" ]; then
    echo "Emscripten SDK not found at ${EMSDK_DIR}. See docs/build.md." >&2
    exit 1
fi
# shellcheck disable=SC1091
source "${EMSDK_DIR}/emsdk_env.sh" >/dev/null 2>&1

emcmake cmake --preset web
cmake --build build/web --target fps_client --parallel

echo "built build/web/game/fps_client.html"
if [ "${1:-}" = "--serve" ]; then
    port="${2:-8099}"
    echo "serving on http://localhost:${port}/fps_client.html (Ctrl-C to stop)"
    python3 -m http.server -d build/web/game "${port}"
fi
