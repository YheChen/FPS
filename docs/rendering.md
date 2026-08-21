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
8. Post-processing: bloom, ACES tonemap, FXAA (M15) ✅
9. Procedural sky + sun disc (M40) ✅
10. Point lights — later, if the maps ever need them
11. Frustum culling — only when the map is big enough to need it

## Post-processing (M15)

The 3D world renders into an off-screen HDR target, then resolves to the
screen through up to five fullscreen passes:

1. **Bright pass** — keep what should bloom, at half resolution
2. **Blur H** and 3. **Blur V** — separable Gaussian
4. **Composite + ACES tonemap** — add bloom, apply exposure, map HDR → LDR
5. **FXAA** — edge-aware smoothing on the tonemapped result

`PostFx::begin_scene()` binds the HDR target; `PostFx::resolve()` runs the
chain. **The HUD and ImGui draw after `resolve()`**, straight to the default
framebuffer, so text is never tonemapped, bloomed or blurred.

`postfx_math.h` holds the curves (`luminance`, `bloom_weight`,
`aces_tonemap`, `half_resolution`) in `engine`, headless and unit-tested.
Note the shaders in `postfx.cpp` implement the same formulas in GLSL — the
tests lock the curve shape down, they cannot prove the GLSL copy matches. If
you change a curve, change both.

### Decisions worth keeping

- **HDR is requested, not assumed.** `RenderTarget::create(..., hdr=true)`
  tries RGBA16F and silently falls back to RGBA8 if the framebuffer is not
  complete. WebGL 2 only supports float colour attachments with
  `EXT_color_buffer_float`, and probing the framebuffer beats parsing
  extension strings — a driver can advertise the extension and still refuse
  the attachment. Ask `hdr()` for what you actually got.
- **A soft knee on the bright pass.** A hard `luminance > threshold` cut
  makes bloom pop on and off as pixels cross it, which is very visible on a
  moving muzzle flash.
- **ACES over Reinhard**, because it keeps saturation in the highlights
  instead of washing bright colours toward white. Be aware the Narkowicz fit
  used here *reaches* 1.0 near an input of 7.8 — it does not asymptote.
- **A fullscreen triangle, not a quad**, drawn from `gl_VertexID` with no
  vertex buffer at all. One primitive, no diagonal seam.
- **`begin_scene()` re-enables the depth test** that `resolve()` turns off
  for its 2D passes. Forgetting that renders the world with no depth test,
  which looks like random geometry sorting.
- **A degenerate size is clamped, not rejected.** The browser canvas reports
  0×0 until it is laid out, so failing on it aborts startup on the web while
  working fine natively.

Colour management is deliberately unchanged from before this milestone:
`GL_FRAMEBUFFER_SRGB` still does the final encode natively, and the LDR
intermediate stores tonemapped *linear* values in 8 bits. That can band in
dark gradients; moving the whole chain to an explicit gamma step is a
separate change.

## Sky (M40)

`eng::Sky` replaces the flat clear colour with a procedural sky: a
horizon-to-zenith gradient plus a sun disc and halo, evaluated in one
fullscreen fragment shader. No cubemap and no texture — a gradient is
cheaper to evaluate than to fetch, and it adds nothing to the `assets/`
bytes every browser player downloads.

The sun is the point of it. `Sky::draw` takes the same direction vector the
lit shader gets as `u_light_direction`, so the disc lands exactly where the
shadows say the light comes from; before this the frame had nothing in it to
explain the shadow direction. Aiming the camera straight down
`normalize(-kSunDirection)` puts the crosshair on the middle of the disc,
which is the check to re-run if either side ever moves.

### Decisions worth keeping

- **Drawn last of the opaque work, not first.** The triangle sits exactly on
  the far plane (`gl_Position.z == w`) and the pass runs with `GL_LEQUAL`, so
  the depth test throws away every fragment the arena already covered and the
  shader only ever runs on sky you can actually see. Drawing it first with
  depth writes off is one line shorter and shades the whole frame, most of it
  for the world to paint over immediately.
- **Into the HDR target, with the rest of the scene.** That is what lets the
  sun feed bloom. Disc radiance is `sun_intensity` (14) — far above the 1.0
  bloom threshold — while the gradient stays below 1.0, because a whole
  hemisphere over the threshold would veil the entire frame.
- **The halo is what blooms, not the disc.** A hard-edged disc two degrees
  across aliases in the half-resolution bright pass; a soft
  `exp(-(1 - cos angle) * falloff)` glow around it does not. `halo_falloff`
  started at 70 and held the sky saturated for 20-odd degrees, which read as
  a white hole rather than a sun; 220 puts the glow at 1/e about 5 degrees
  out and gone by 12.
- **Depth state is restored, not saved.** `Sky::draw` puts `GL_LESS` and
  depth writes back afterwards, the same way `ParticleRenderer::draw` does —
  `PostFx::begin_scene` is what re-enables the depth *test* each frame.
- **The clear colour is now black**, and is never meant to be seen. Flat
  black in a frame means the sky pass did not run, rather than "that is the
  background".

### No distance fog

Considered and rejected. Both arenas are sealed boxes with 4 m walls: the
longest sight line in arena01 is the 56 m diagonal, and the horizon is never
visible from inside one, so fog has nothing to blend the geometry into. Fog
weak enough not to touch a 30 m duel would be invisible; fog strong enough
to see would be lifting the darkest pixels of an enemy silhouette at exactly
the ranges where targets are acquired. Readability wins.

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

Muzzle flash needs its own tuning: the muzzle sits well under a metre from
the camera, so sizes and speeds that look right out in the arena become
screen-filling drifting orbs there. Where it spawns is not a constant — it
comes from the held weapon's own `muzzle` marker node (see below).

## First-person viewmodel (M46)

The held weapon is drawn from the camera basis: local +X is the camera's
right, +Y its up, -Z where it is looking, and the origin is the grip. Bob,
sway, recoil, reload and switch are all offsets and rotations **about the
grip** on top of that, which is why one set of them serves five weapons —
every model in `assets/weapons/*.glb` puts its origin in the hand.

Right and down are fixed for the arsenal; **forward is not**. The arm
extends until the weapon's business end is a fixed distance from the eye, so
a 1.30 m sniper is held at the shoulder and a 0.33 m knife out in front,
with no per-weapon constant. Reach comes from the model's own bounds.

Four rendering decisions worth keeping:

- **Its own projection, after a depth clear.** Same FOV and aspect as the
  camera, near plane 0.01 instead of 0.05. The clear is the anti-clipping
  mechanism — there is no world depth left to lose against, so walking into
  a wall puts the gun in front of the wall rather than a barrel through it.
  The FOV *must* match, or a point projects to a different pixel in the two
  matrices and the flash lands somewhere other than the drawn muzzle.
- **After the particles and debug lines, not before.** They still need the
  world's depth buffer to be occluded by the world; clearing it first would
  put impact sparks through pillars. The cost is that the gun overdraws the
  part of its own flash that falls on the barrel, which is what a real flash
  does anyway.
- **Inside the HDR target**, so the flash still feeds bloom.
- **Casts no shadow, receives them.** It is not in `draw_items`, so the
  depth pass never sees it — a gun-shaped shadow on the floor beside a
  player who cannot see their own body would be worse than none. Its
  transform is a genuine world position, so the lit pass shadows it for
  free and the gun darkens when you step into a pillar's shade.

The **`muzzle` marker node** is what keeps weapon identity out of the
client: a mesh-less node read by name, exactly like a map's `spawn_N`. Its
absence is meaningful — a melee weapon has none, and that is the whole
mechanism preventing a knife swing from spitting fire. Recoil is scaled from
the weapon's own config (damage × pellets), so a new `.cfg` gets a kick that
matches its stats without a line of code.

Everything here is **render-clock cosmetic** and must never feed the fixed
tick — the same rule particles follow. Recoil deliberately does not move the
camera: aim punch would change where bullets go, which is simulation.

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

`--fixed-yaw <radians>` and `--fixed-pitch <radians, + is up>` lock the view
direction so a screenshot run can aim at whatever the change under test
needs to show. Both apply offline and online. Pointing them at
`normalize(-kSunDirection)` — `--fixed-yaw 2.2143 --fixed-pitch 1.107` for
the current sun — is how the sky's agreement with the light gets checked.

`--weapon <1-5>` seeds the requested slot, which is the only way to put a
chosen gun in front of an automated screenshot — the 1-5 keys need a
keyboard. `--auto-walk` holds forward, which the viewmodel's bob needs
because the bob is driven by distance travelled and a stationary player has
none. `--auto-fire` PULLS the trigger every other tick rather than holding
it down: held, a semi-automatic weapon fires exactly once and then sits
there, so nothing that needed a shotgun or a sniper to keep shooting could
be verified at all. All three apply offline and online.

`--screenshot-at <seconds>` moves the capture off the final frame, which is
the only frame `--screenshot` alone can reach. Anything that exists only
*during* a transition — sights rising, a weapon coming up, a reload arcing, a
map rotating — is finished long before a run ends, so its evidence could not
be captured at all. The deadline is **seconds into rendering**, not since
process start: loading assets and opening a window costs over a second, and
every value below that used to land on frame 1.

```bash
# the sniper's 0.7 s raise, three frames along the clip
for t in 0.0 0.25 0.55; do
  ./build/debug/game/fps_client --weapon 4 --run-seconds 2 --no-vsync \
      --screenshot-at $t --screenshot /tmp/raise-$t.png
done
```

One caveat with teeth: `FixedTimestep::kMaxPendingTicks` is 8, so the first
rendered frame already contains up to 0.133 s of simulation. A transition
shorter than that — the rifle's 0.20 s sight raise is close — is mostly over
before any frame exists, and `--screenshot-at 0` is as early as it gets.
Verifying a *fast* transition means slowing it down in the config first.

`--connect <host:port>` is accepted as well as `--connect <host> --port <n>`,
because one address split across two flags is a thing to get wrong every
time. A value carrying a scheme is left alone: `rtc://host:7777` keeps its
port inside the string, because the WebRTC transport signals over the `ws://`
form of that whole URL and never reads `--port`.

The **server** has `--bot-weapon <1-5>`, which is the same idea for the other
side of the wire. Bots were pinned to slot 0, so verifying anything about the
smg, shotgun, sniper or knife in a real match meant editing a `.cfg` and
remembering to revert it. It is a verification hook, not a difficulty knob —
`--bot-skill` is that.

**Every one of these hooks has to be wired into BOTH the offline and online
branches**, and that is not a style note. `--fixed-yaw` and `--auto-fire`
were each silently offline-inert once, and aim-down-sights itself was
invisible online for four milestones because the one code path that advanced
it was the offline weapon tick — while every screenshot ever taken of the
feature was an offline one. The evidence was real and it proved the wrong
path.

**A clean emcc build does not mean the web client works.** Shader
compilation happens at runtime, so GLSL ES errors only surface in a browser.
M13's missing `sampler2DShadow` precision built without a warning and broke
every web client. Serve the build and check the console:

```bash
cd build/web/game && python3 -m http.server 8931
```
