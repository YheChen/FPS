# Rendering

Status: design stub — implementation starts in Milestone 1 (context) and
Milestone 2 (renderer). This document grows with the code.

## Baseline

- **OpenGL 4.1 core profile** everywhere. macOS caps OpenGL at 4.1
  (deprecated but functional), and one API level on all platforms beats
  per-platform paths. Consequences: no compute shaders, no DSA, no
  `glDebugMessageCallback` (use `glGetError` sweeps in Debug builds).
- Loader: glad (GL 4.1 core, no extensions initially).
- The renderer sits behind a small engine-owned API (`Shader`, `Mesh`,
  `Texture2D`, `Camera`, `DebugDraw`); game code never calls GL directly.
  This keeps a later backend swap (GL 4.6 path or Metal/Vulkan) contained.

## Conventions

- Right-handed, +Y up, -Z forward (matches glTF).
- Units: meters. Depth: standard [-1, 1] clip (GL default), reversed-Z only
  if depth precision ever becomes a real problem.
- Colors: sRGB framebuffer; albedo textures sRGB, data textures linear.

## Planned progression

1. Clear screen (M1)
2. Triangle → textured cube (M2)
3. Many meshes, camera, Blinn-Phong directional light, debug lines (M2)
4. glTF environment, materials (M3)
5. Point lights, simple shadow map for the sun — later, after gameplay
6. Frustum culling — only when the map is big enough to need it
