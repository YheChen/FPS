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

# FPS_WEB_SERVER_URL (optional) bakes the deployed server address into the
# client menu, e.g. FPS_WEB_SERVER_URL=wss://fps.example.com scripts/build_web.sh
if [ -n "${FPS_WEB_SERVER_URL:-}" ]; then
    emcmake cmake --preset web "-DFPS_WEB_SERVER_URL=${FPS_WEB_SERVER_URL}"
else
    emcmake cmake --preset web
fi
cmake --build build/web --target fps_client --parallel

# Stage the Vercel config alongside the artifacts for `vercel deploy`.
cp web/vercel.json build/web/game/vercel.json

echo "built build/web/game/fps_client.html"
if [ "${1:-}" = "--serve" ]; then
    port="${2:-8099}"
    echo "serving on http://localhost:${port}/fps_client.html (Ctrl-C to stop)"
    python3 -m http.server -d build/web/game "${port}"
fi
