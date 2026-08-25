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

# Bounded on purpose. A bare `--parallel` uses every hardware thread, and
# Jolt's template-heavy translation units want ~1 GB each under GCC -- so on a
# 12-thread laptop the build asks for more memory than the machine has and the
# OOM killer takes cc1plus, and sometimes sshd with it. Four jobs peaks around
# 4-6 GB and still finished this build in 105 s where the unbounded version
# died after 10 minutes.
#
# Raise it with --build-arg BUILD_JOBS=8 on a machine with memory to spare.
ARG BUILD_JOBS=4

# FPS_BUILD_CLIENT=OFF is required, not merely tidy: SDL3's CMake hard-errors
# during CONFIGURE when no X11/Wayland development libraries are present, even
# though fps_server never links it. See docs/deploy.md.
RUN cmake --preset release -DFPS_BUILD_CLIENT=OFF -DFPS_BUILD_TESTS=OFF \
    && cmake --build --preset release --target fps_server --parallel "${BUILD_JOBS}"

# --- runtime ----------------------------------------------------------------
FROM fedora:latest

# python3 is here for the health check below, which speaks the real game
# protocol rather than just proving a port is open.
#
# The C++ runtime is UPGRADED, not installed, and the difference is the whole
# bug. Both stages say `fedora:latest`, which is not enough to make them
# agree: the build stage installs gcc-c++ and gets libstdc++ upgraded as a
# dependency, while the runtime stage keeps whatever the base image shipped.
# The server then builds cleanly and refuses to start:
#
#     /opt/fps/fps_server: /lib64/libstdc++.so.6:
#         version `GLIBCXX_3.4.36' not found (required by /opt/fps/fps_server)
#
# which surfaces as the health check never going healthy, with a green build
# directly above it.
#
# `dnf install libstdc++` does NOT fix it -- the package is already present,
# so install is a no-op and leaves the old one in place. Measured in the base
# image:
#
#     base                        libstdc++.so.6.0.35
#     dnf install -y libstdc++    libstdc++.so.6.0.35   <- unchanged
#     dnf upgrade -y libstdc++    libstdc++.so.6.0.36
#     dnf install -y gcc-c++      libstdc++.so.6.0.36   <- what the build has
#
# libgcc comes along for the ride because it carries its own symbol versions
# and drifts with the same compiler.
RUN dnf install -y python3 \
    && dnf upgrade -y libstdc++ libgcc \
    && dnf clean all \
    && useradd --system --home /opt/fps --shell /usr/sbin/nologin fps

# The stats volume mounts here. The directory has to exist IN THE IMAGE, owned
# by the user the server runs as: Docker initializes a new named volume from
# the image's ownership at that path, and when the path is absent it creates
# it root-owned instead. The server then runs as uid 999 against a root:root
# 0755 directory and every save fails with "cannot write ... .tmp" -- which is
# exactly what the first real deploy did.
RUN install -d -o fps -g fps -m 0755 /var/lib/fps

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
