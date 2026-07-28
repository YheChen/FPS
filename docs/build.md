# Building

## Prerequisites

- CMake ≥ 3.25
- A C++23 compiler: AppleClang 15+, Clang 17+, GCC 13+, or MSVC 2022 (17.8+)
- Git (dependencies are fetched at configure time via `FetchContent`)
- Internet access on first configure (Catch2 download)

## Standard workflow (presets)

```sh
cmake --preset debug            # configure into build/debug
cmake --build --preset debug --parallel
ctest --preset debug            # run unit tests
```

Other presets:

| Preset    | Purpose                              |
|-----------|--------------------------------------|
| `debug`   | Debug build, assertions enabled      |
| `release` | Optimized build, assertions disabled |
| `asan`    | Debug + AddressSanitizer + UBSan     |
| `web`     | WebAssembly client (Emscripten)      |

## Web (WebAssembly) client

The client compiles to WebAssembly + WebGL 2 with Emscripten (the server is
native-only). Install the SDK once:

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
```

Then build and serve it:

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake --preset web           # configure into build/web
cmake --build build/web --target fps_client --parallel
python3 -m http.server -d build/web/game 8099    # serve
# open http://localhost:8099/fps_client.html
```

Outputs `fps_client.{html,js,wasm,data}` — the `.data` bundles `assets/`
into the virtual filesystem. A browser tab cannot open the ENet/UDP transport,
so the web client joins over WebSockets: point it at a `ws://` or `wss://` URL
(the menu defaults to `ws://localhost:7778`) and run the server with
`--ws-port`. Deploying it publicly is covered in [deploy.md](deploy.md).

CI builds this and runs it in a browser on every PR; see
[the browser smoke test](#the-browser-smoke-test).

## Options

| CMake option              | Default | Effect                                  |
|---------------------------|---------|-----------------------------------------|
| `FPS_BUILD_TESTS`         | ON      | Build the Catch2 unit test target       |
| `FPS_ENABLE_SANITIZERS`   | OFF     | ASan + UBSan on all project targets     |
| `FPS_WARNINGS_AS_ERRORS`  | OFF     | `-Werror` / `/WX` (always ON in CI)     |
| `FPS_ENABLE_WEBRTC`       | OFF     | Native WebRTC DataChannel transport     |

## Running

```sh
./build/debug/game/fps_client
./build/debug/game/fps_server
./build/debug/tests/engine_tests   # or via ctest
```

## Formatting and static analysis

```sh
# Format everything in place
find engine game tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i

# clang-tidy (uses build/debug/compile_commands.json)
clang-tidy -p build/debug engine/core/log.cpp
```

## Before opening a PR

A clean local build is **not** enough to predict CI. Local development is
AppleClang + libc++; CI also runs GCC (Ubuntu) and MSVC (Windows), both of
which reject code that AppleClang accepts. Two checks close most of that
gap, and both are fast:

```sh
python3 tools/check_includes.py   # std:: symbols used without their header
python3 tools/gcc_check.py        # every TU through real GCC, CI's warning set
```

`check_includes.py` catches the libc++ trap: libc++ pulls `<algorithm>` in
transitively, so `std::clamp` without the include builds on macOS and fails
on MSVC. It also runs in CI's format-check job.

`gcc_check.py` needs `brew install gcc`. It compiles each translation unit
with `-fsyntax-only` using that file's own flags from
`build/debug/compile_commands.json`, so nothing links and SDL's
Objective-C is never involved. It catches the GCC-only diagnostics that
AppleClang stays quiet about — `-Wsign-conversion` on `int` → `size_t`,
`-Wrange-loop-construct` on structured-binding copies. Note it reports on
*diagnostics*, not exit status: without `-Werror` a warning still exits 0,
and warnings are exactly what CI promotes to errors.

Neither covers the browser. CI does (see below), but if a change touches
rendering or the platform layer it is worth running locally first, because the
turnaround is minutes rather than a CI round trip:

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake --preset web -DFPS_WARNINGS_AS_ERRORS=ON
cmake --build --preset web --parallel
node tools/web_smoke.mjs                  # loads it in headless Chrome
node tools/web_smoke.mjs --zero-canvas     # ...and again, canvas starved
```

## The browser smoke test

`tools/web_smoke.mjs` builds nothing; it loads an already-built
`build/web/game/fps_client.html` in headless Chrome and checks that the client
is really running. It needs Node 22+ (for the built-in `WebSocket` it uses to
drive the DevTools protocol) and a system Chrome or Chromium. No `npm install`.

It exists because **a green Emscripten build predicts almost nothing.** Every
web-only defect this project has shipped compiled cleanly and broke at runtime:

| Defect | What the compiler saw | What the browser did |
|---|---|---|
| `sampler2DShadow` with no precision qualifier | fine (desktop GLSL has defaults) | every shader using it failed to compile |
| 0×0 canvas at startup | fine | `PostFx::create` failed, client exited before drawing |
| dynamically indexed uniform array in the skinning shader | fine | ~1000× slower; one draw call at 1.5 s |
| `web` preset with no toolchain file | fine | built a **native** binary; "web build is clean" was vacuous |

So it checks, in order: the artifacts are a real wasm module (not a native
binary); the client reaches its frame loop; the framebuffer contains more than
one colour; and the frame rate is not pathological. Each of those thresholds is
deliberately loose — they separate "working" from "catastrophically broken",
which is the only honest distinction a software rasteriser on a shared runner
can make. Do not tighten them into a performance benchmark; that just buys
flakiness.

`--zero-canvas` holds the canvas at zero height until the client has been seen
running, then restores it. A plain headless load lays the canvas out before the
module boots, so it never reproduces the startup race — the 0×0 bug passes the
ordinary run and only fails this one.

Useful flags: `--verbose` echoes the page console and Chrome's own log,
`--keep-open` leaves the browser up, `--dir` points at another build tree.

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and PR:

1. Debug build + tests on Ubuntu, macOS, and Windows, warnings-as-errors.
2. Ubuntu build + tests under ASan/UBSan.
3. Ubuntu build + tests with `FPS_ENABLE_WEBRTC=ON`.
4. Emscripten build (warnings-as-errors) + the browser smoke test, both modes.
5. clang-format check + the standard-header check.

Warnings-as-errors on the web build earns its keep separately from the native
jobs: `size_t` is 32 bits on wasm32, so a `uint64_t` → `size_t` conversion that
is lossless on every other target is real truncation there. Enabling it found
three such conversions in the WebSocket frame parser.

**Check the run on `main` after merging, not just the PR run.** A PR can be
green and still leave `main` red if the merge combines with another change
— that happened once already (M11).
