# ADR 0003 — OpenGL 4.1 core as the rendering baseline

Status: accepted (2026-07-15)

## Context

macOS deprecates OpenGL and caps it at 4.1 core profile. Windows/Linux could
use 4.6, but per-platform renderer paths contradict the project's
"small and understandable" goal, and Vulkan/Metal before a working game loop
is an explicit anti-goal.

## Decision

Target OpenGL 4.1 core profile on all platforms, loaded via glad. All GL
usage stays behind the engine's rendering API; game code never calls GL.

## Consequences

- No compute shaders, no direct state access, no `glDebugMessageCallback`
  (Debug builds use `glGetError` sweeps instead).
- Works today on all three targets with one code path.
- Apple may remove GL eventually; the thin renderer API is the planned
  migration seam (GL 4.6 / Metal / Vulkan backend), not a rewrite of game
  code. Revisit only after the game is playable.
