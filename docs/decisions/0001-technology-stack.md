# ADR 0001 — Technology stack

Status: accepted (2026-07-15)

## Context

We need focused libraries for a small cross-platform (Windows/macOS/Linux)
multiplayer FPS on a custom engine, favoring understandability and build
simplicity over feature count.

## Decision

| Concern        | Choice | Notes |
|----------------|--------|-------|
| Language       | C++23 | `std::expected`, `std::format`; features limited to what AppleClang / GCC 13 / MSVC 2022 all support |
| Build          | CMake ≥ 3.25 + FetchContent, presets | dependencies pinned by tag |
| Window/Input   | **SDL3** (over GLFW) | one dependency covers window, input, relative mouse, gamepads, and misc platform glue; GLFW would need supplements later |
| Graphics       | OpenGL **4.1 core** + glad | see ADR 0003 |
| Math           | GLM | |
| UI/Debug       | Dear ImGui | |
| Physics        | Jolt Physics | `CharacterVirtual` for the player (ADR 0004) |
| Audio          | miniaudio | single file, owns the device thread |
| Networking     | **ENet** (over SteamNetworkingSockets) | tiny C library, exactly the reliable+unreliable channel semantics we need, trivial to build on all 3 OSes. GNS adds crypto/build weight and mainly shines with Steam integration. Tradeoffs accepted: no encryption, no NAT traversal — fine for a LAN/direct-IP scope. The transport sits behind `eng::net::Transport` so GNS can replace it later without touching game code. |
| glTF loading   | **cgltf** + stb_image (instead of Assimp) | Assimp is a very large dependency that slows builds and imports 40 formats we don't use. cgltf is a small single-file parser for our one chosen format. Tradeoff: glTF only, and we write the scene mapping ourselves (which we want to control anyway). |
| Testing        | Catch2 v3 | |
| Logging        | Hand-rolled (~100 lines) instead of spdlog | the project's goal is an understandable engine; our needs are levels + timestamps + thread safety. Tradeoff: no rotating files/async sinks — acceptable. |

## Consequences

- All dependencies are fetched and pinned via CMake; no system packages
  required beyond a compiler.
- No encryption on the wire until/unless the transport is swapped.
- Asset pipeline is glTF-only by construction.
