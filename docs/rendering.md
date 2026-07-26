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
6. Directional shadow map for the sun (M13) ✅
7. Particles (M14) ✅
8. Point lights — later, if the maps ever need them
9. Frustum culling — only when the map is big enough to need it

## Particles (M14)

Split the same way as shadows: `ParticlePool` (`engine`, headless, unit
tested) owns the simulation; `ParticleRenderer` (`engine_platform`) draws it
as camera-facing quads in **one instanced draw call**.

Simulated on the CPU, not in a compute shader — OpenGL 4.1 and WebGL 2 have
no compute (ADR 0003). A few thousand particles is nowhere near a CPU budget
problem at 60 Hz, and the alternative (transform feedback) buys nothing at
this scale.

### Premultiplied alpha, one pass

Blending is `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` and colors are premultiplied.
That gets both looks a shooter needs out of a single pass and a single
batch:

- **alpha 0, bright RGB** → pure additive glow (muzzle flash, sparks)
- **alpha > 0** → an opaque puff (dust, blood)

Without this the system would need two batches and a sort between them.

The sprite is a computed radial falloff, not a texture — one less asset to
ship for no visual difference at these sizes.

### Other decisions worth keeping

- Particles **test** depth but do not **write** it. Writing depth would make
  particles from the same burst punch holes in each other.
- The instance buffer is orphaned (`glBufferData(..., nullptr, ...)`) before
  each upload so the driver hands back fresh storage instead of stalling on
  the previous frame's draw.
- Drag is exponential (`v *= exp(-drag*dt)`), so behaviour does not change
  with frame rate. A linear `1 - drag*dt` damping would.
- A full pool **drops** the excess rather than growing. An effect firing
  every frame must not be able to consume memory without bound.
- The pool has its own hash, deliberately **not** `game/shared/rng.h`.
  Gameplay randomness has to stay bit-exact for prediction and replay, and
  nothing cosmetic should be able to reach into it. Particles also advance
  on the render clock, not the fixed tick, for the same reason.

Muzzle flash needs its own tuning: the muzzle sits ~0.5 m from the near
plane, so sizes and speeds that look right out in the arena become
screen-filling drifting orbs there. It is also offset forward/right/down,
since there is no first-person weapon model and a flash at dead centre reads
as a light in the player's face.

## Shadows (M13)

One directional light, one 2048² depth map, no cascades. The arena is ~45 m
across, so a single tightly fitted projection gives roughly 2 cm per texel;
cascades only pay for their complexity on maps too large for one projection.

The frame is two passes over **one** draw list. Building the list once
(`DrawItem`, in the client) is deliberate: a caster present in the lit pass
but missing from the depth pass is the classic shadow bug, and sharing the
list makes that impossible by construction.

1. **Depth pass** — bind the shadow map, render every caster with a
   position-only shader from the light's point of view.
2. **Lit pass** — normal render, sampling the depth map to decide whether
   the sun reaches each fragment.

`directional_light_view_projection` (in `engine/rendering/light.h`, headless
and unit-tested) fits an orthographic box to the scene bounds as seen from
the light. It is the part worth testing, so it lives in `engine` rather than
`engine_platform`: the tests assert that every corner of the bounds lands
inside NDC *and* that the fit is tight, which a merely-covering projection
would fail.

### Acne, and what fixes it here

Three mitigations, because no single one is enough:

- **Front-face culling in the depth pass.** The recorded depth is the
  caster's back face, so self-shadowing errors land on surfaces already
  facing away from the light.
- **Slope-scaled bias.** Surfaces edge-on to the light span more depth per
  texel, so the bias grows with `1 - dot(normal, light)`. A constant bias
  large enough for grazing angles causes visible peter-panning everywhere
  else.
- **Hardware comparison sampling** (`GL_COMPARE_REF_TO_TEXTURE` +
  `sampler2DShadow`), which gives 2×2 filtering for free; the shader adds a
  3×3 PCF kernel on top.

Ambient light is deliberately **not** shadowed — only the sun's direct
contribution is occluded. Shadowed areas stay readable instead of going
black, which matters in a game where players hide in them.

### WebGL 2 notes

`GL_DEPTH_COMPONENT24` + `GL_UNSIGNED_INT`, `GL_COMPARE_REF_TO_TEXTURE` and
`sampler2DShadow` are all core in GLES 3.0, so the same path runs in the
browser. Two divergences:

- A depth-only framebuffer needs the draw buffer switched off. Desktop uses
  `glDrawBuffer(GL_NONE)`; GLES has no singular form, so the web path uses
  `glDrawBuffers(1, {GL_NONE})`.
- **GLSL ES has no default precision for `sampler2DShadow`**, so the shader
  fails to compile with "No precision specified" unless `glsl_preamble()`
  declares one. This compiles fine under emcc and only fails when the
  shader is actually linked in a browser — see the verification note below.

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

`--fixed-yaw <radians>` locks the view direction so a screenshot run can aim
at whatever the change under test needs to show.

**A clean emcc build does not mean the web client works.** Shader
compilation happens at runtime, so GLSL ES errors only surface in a browser.
M13's missing `sampler2DShadow` precision built without a warning and broke
every web client. Serve the build and check the console:

```bash
cd build/web/game && python3 -m http.server 8931
```
