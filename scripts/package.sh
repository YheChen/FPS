#!/usr/bin/env bash
# Builds a release configuration and stages a runnable game package:
#   dist/fps-<os>-<arch>/ { fps_client, fps_server, assets/ } (+ .zip)
set -euo pipefail

cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --parallel

os="$(uname -s | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
name="fps-${os}-${arch}"
stage="dist/${name}"

rm -rf "${stage}"
mkdir -p "${stage}"
cp build/release/game/fps_client "${stage}/"
cp build/release/game/fps_server "${stage}/"
cp -R assets "${stage}/assets"
cat > "${stage}/README.txt" <<'EOF'
FPS - engine + game prototype

Server:  ./fps_server --port 7777
Client:  ./fps_client            (menu: enter the server address and Connect)

Run both from this directory (the game locates ./assets relative to the
working directory).
EOF

(cd dist && zip -qr "${name}.zip" "${name}")
echo "packaged dist/${name}.zip"
