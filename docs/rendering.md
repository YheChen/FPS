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

1. Clear screen (M1) ✅
2. Triangle → textured cube (M2) ✅
3. Many meshes, camera, Blinn-Phong directional light, debug lines (M2) ✅
4. glTF environment, materials (M3) ✅
5. Textured materials from glTF images (M12) ✅
6. Simple shadow map for the sun (M13)
7. Point lights — later, if the maps ever need them
8. Frustum culling — only when the map is big enough to need it

## Textures and materials (M12)

`load_gltf` decodes embedded images to RGBA8 via stb_image, and the client
uploads each one to a `Texture2D` that stays **index-aligned with
`GltfModel::images`**, so a material's `base_color_image` indexes the GPU
list directly. An image that fails to decode gets the magenta
missing-texture checkerboard rather than silently rendering untextured.

Three details that are easy to get wrong:

- **Color space.** Base color images are sRGB-encoded but the lighting math
  is linear, so they upload as `GL_SRGB8_ALPHA8` and the hardware converts
  on sample. Data textures (normals, roughness, masks) must *not* do this.
- **No vertical flip.** glTF's UV origin is top-left, stb decodes top row
  first, and GL treats the first row as `t = 0`. The two conventions cancel;
  flipping (the usual reflex) would be wrong here.
- **UVs are authored in world units.** `tools/gen_arena.py` scales each
  box's UVs by its real size (`TEXTURE_SCALE` texels/m) instead of a 0..1
  unwrap, so texel density is constant and a 40 m floor tiles ~20 times
  rather than stretching one texture across it. This is why the arena emits
  one mesh per box: the UVs differ per box even where the material doesn't.
  Samplers use `REPEAT` wrap, and the source textures are generated seamless
  (`tools/gen_textures.py` wraps its noise lattice modulo the tile size).

Texture binds are the only per-draw GL state in the scene pass, so the draw
loop tracks the bound texture and only switches when the material changes.

### Headless loads

`AssetCache` takes a `decode_images` flag. The dedicated server passes
`false`: it needs collision geometry, never pixels, and decoding is the
expensive part of loading a map. Image *slots* still survive the skip
(`GltfImage` entries with no pixels) so material indices stay meaningful.

## Verifying rendering changes

`eng::save_framebuffer_png` reads the back buffer and writes a PNG. The
client exposes it as `--screenshot <path>`, captured on the final frame
before the `--run-seconds` quit:

```bash
./build/debug/game/fps_client --run-seconds 3 --no-vsync --screenshot /tmp/shot.png
```

It stalls the pipeline on `glReadPixels`, so it is a verification and
bug-report tool, not something to call in a hot loop. Rendering milestones
are expected to attach one of these rather than assert "it looks right".
