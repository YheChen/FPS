# Dedicated server image. Fedora on both stages to match the deployment host
# (docs/deploy.md); nothing here is Fedora-specific beyond the package names.
#
# Build from the REPOSITORY ROOT, not from deploy/:
#     docker build -f deploy/Dockerfile -t fps-server .

# --- build ------------------------------------------------------------------
FROM fedora:latest AS build

# Same package list as the bare-metal instructions in docs/deploy.md.
RUN dnf install -y git cmake ninja-build gcc-c++ make ca-certificates \
    && dnf clean all

WORKDIR /src
COPY . .

# FPS_BUILD_CLIENT=OFF is required, not merely tidy: SDL3's CMake hard-errors
# during CONFIGURE when no X11/Wayland development libraries are present, even
# though fps_server never links it. See docs/deploy.md.
RUN cmake --preset release -DFPS_BUILD_CLIENT=OFF -DFPS_BUILD_TESTS=OFF \
    && cmake --build --preset release --target fps_server --parallel

# --- runtime ----------------------------------------------------------------
FROM fedora:latest

# python3 is here for the health check below, which speaks the real game
# protocol rather than just proving a port is open.
RUN dnf install -y python3 \
    && dnf clean all \
    && useradd --system --home /opt/fps --shell /usr/sbin/nologin fps

WORKDIR /opt/fps

COPY --from=build /src/build/release/game/fps_server /opt/fps/fps_server
# find_assets_root() walks up from the working directory, so assets/ must sit
# next to the binary.
COPY --from=build /src/assets /opt/fps/assets
# ws_smoke.py reads the protocol version out of the header, so the header comes
# along at the path it expects (tools/../game/shared/protocol.h). That is what
# keeps the health check from drifting out of sync with the wire format.
COPY --from=build /src/tools/ws_smoke.py /opt/fps/tools/ws_smoke.py
COPY --from=build /src/game/shared/protocol.h /opt/fps/game/shared/protocol.h

USER fps

# WebSocket (browser players). 7777/udp is ENet for native players; it is not
# exposed here because the default command passes --no-enet.
EXPOSE 7778

# A TCP connect would only prove something is listening. This completes the
# WebSocket upgrade and a ClientHello/ServerWelcome exchange, so a server that
# is up but wedged fails the check.
HEALTHCHECK --interval=30s --timeout=10s --start-period=15s --retries=3 \
    CMD python3 /opt/fps/tools/ws_smoke.py 127.0.0.1 7778 || exit 1

ENTRYPOINT ["/opt/fps/fps_server"]
CMD ["--ws-port", "7778", "--no-enet"]
