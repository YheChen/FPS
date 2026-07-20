# ADR 0005 — Web browser client (Emscripten + WebSocket)

Status: accepted (2026-07-17) — implemented in Milestone 10

## Context

The native client (SDL3 + desktop OpenGL) requires a download and a
type-in-IP connect, which is most of the friction for a hobby FPS. Running
in a browser removes both. Two hard constraints shape the design:

1. Browsers cannot open UDP sockets, so the ENet transport cannot run in a
   tab.
2. A page served over HTTPS (e.g. Vercel) may only open `wss://` (secure)
   WebSockets — mixed content blocks `ws://`.

## Decision

- Compile the **client** to WebAssembly with **Emscripten**, rendering
  through **WebGL 2 (OpenGL ES 3.0)**. The engine's small GL abstraction and
  the engine/game split make this a contained port: one `engine/rendering/gl.h`
  shim selects glad (desktop) or `<GLES3/gl3.h>` (web), shaders omit their
  `#version` line and get a platform preamble, and the main loop runs under
  `emscripten_set_main_loop`.
- Transport is behind interfaces (`IServerTransport`, and later a client
  transport seam). The browser client speaks **WebSockets**; the dedicated
  server already accepts them via `WebSocketHost` (ADR-adjacent, M10a), and a
  `CompositeTransport` lets ENet (native) and WebSocket (browser) players
  share one match.
- WebSockets are reliable + ordered (TCP). We accept the resulting
  head-of-line jitter for now rather than build WebRTC DataChannels: the
  transport seam means WebRTC can replace the browser transport later without
  touching game code. Measure before investing.
- **TLS is terminated by a reverse proxy** (Caddy) on the server host, so the
  C++ server stays plain `ws://` on localhost and the browser reaches
  `wss://` through the proxy (M10c).

## Consequences

- The server stays authoritative and unchanged; only transport is added.
- WebGL 2 lacks a few desktop tokens (`GL_FRAMEBUFFER_SRGB`); those call
  sites are guarded. No compute/geometry shaders were used, so nothing else
  is lost.
- Deploying the browser client requires a domain name for the TLS cert
  (Let's Encrypt cannot issue for a bare IP).
- Two render backends (GL 4.1 core, GLES 3.0) now exist behind one API; both
  must keep working. The shader common-subset discipline is the cost.
