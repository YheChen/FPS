# FPS Engine

A small, understandable custom game engine in C++23, and an online multiplayer
first-person shooter built on top of it. Authoritative dedicated server,
client-side prediction, snapshot interpolation.

This is **not** an attempt at a general-purpose AAA engine. Every system exists
because the FPS needs it.

## Layout

```
engine/   Reusable engine library (no game-specific code)
game/     The FPS: shared simulation code, client, dedicated server
tests/    Unit tests (Catch2)
docs/     Architecture, networking, milestones, decision records
cmake/    Build helper modules
```

## Build

Requires CMake ≥ 3.25 and a C++23 compiler (AppleClang 15+, GCC 13+, MSVC 2022).

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Binaries land in `build/debug/game/`: `fps_client` and `fps_server`.

See [docs/build.md](docs/build.md) for presets, sanitizers, and CI details.

## Status

Milestone 0 (project foundation) complete. See
[docs/milestones.md](docs/milestones.md) for the roadmap.
