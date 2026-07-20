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
into the virtual filesystem. Currently the browser build plays the offline
practice range; online multiplayer over WebSockets is the next slice
(browser tabs cannot open the ENet/UDP transport). Deploying it publicly is
covered in [deploy.md](deploy.md).

## Options

| CMake option              | Default | Effect                                  |
|---------------------------|---------|-----------------------------------------|
| `FPS_BUILD_TESTS`         | ON      | Build the Catch2 unit test target       |
| `FPS_ENABLE_SANITIZERS`   | OFF     | ASan + UBSan on all project targets     |
| `FPS_WARNINGS_AS_ERRORS`  | OFF     | `-Werror` / `/WX` (always ON in CI)     |

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

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and PR:

1. Debug build + tests on Ubuntu, macOS, and Windows, warnings-as-errors.
2. Ubuntu build + tests under ASan/UBSan.
3. clang-format check.
