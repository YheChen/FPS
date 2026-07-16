# Coding standards and ownership conventions

## Language and style

- **C++23**, no compiler extensions. Prefer features with full support on
  AppleClang, GCC 13, and MSVC 2022 (e.g. `std::expected` is allowed;
  `std::print` is not yet).
- Formatting is mechanical: `.clang-format` decides, nobody argues.
- Naming:
  - Types: `PascalCase` (`SnapshotBuffer`, `CharacterController`)
  - Functions, variables, namespaces, files: `snake_case`
  - Member variables: trailing underscore in classes (`tick_rate_`); plain
    names in aggregates/structs
  - Constants: `kPascalCase`
  - Macros: `ENG_` prefix, used only where a function cannot work
    (assertions, conditional compilation)
- Namespaces: engine code in `eng` (`eng::log`, `eng::net`, …); game code in
  `game` (`game::shared`, `game::client`, `game::server`).
- Includes are repo-root-relative: `#include "engine/core/log.h"`.
- `enum class` always. `const` correctness everywhere. `explicit` on
  single-argument constructors.

## Ownership and lifetime

Default rules, in order of preference:

1. **Values and containers of values.** Most data is plain structs owned by
   value in a system that updates them.
2. **`std::unique_ptr`** when a lifetime is dynamic or a type is expensive or
   non-movable. Ownership is single and explicit.
3. **References / raw pointers / `std::span` are non-owning borrows.** A raw
   pointer never owns. Borrows must not outlive the owner; owners are
   documented below.
4. **`std::shared_ptr` is exceptional** and requires a written justification
   in a comment. Expected uses: none yet.
5. No `new`/`delete`, no `malloc`, no owning raw pointers.

### Ownership map (kept current as systems appear)

| Object                     | Owner                                        |
|----------------------------|----------------------------------------------|
| `Application`              | `main()` (stack value)                       |
| `Window`, GL context       | `Application`                                |
| `Renderer`                 | `Application`; borrows `Window`              |
| `AssetCache` + assets      | `Application`; systems hold typed handles, never raw asset pointers |
| `Scene` / entities         | The game session; entities are IDs (index + generation), components live in arrays owned by the scene |
| Physics world (Jolt)       | The game session (client and server each own one) |
| Network host/connections   | `ClientNetwork` / `ServerNetwork`; peers referenced by connection ID, never stored pointers |
| Game session (match state) | Server: `ServerApp`; Client: `ClientApp`     |
| Prediction history         | `ClientPrediction` (fixed-size ring buffer, value-owned) |

### Global state

Exactly one sanctioned global: the logger's minimum level (`eng::log`).
Everything else is passed explicitly. If you feel the need for a singleton,
pass a reference instead.

## Threading

- The simulation, rendering, audio-control, and networking code all run on
  the **main thread**. This is an explicit invariant until profiling proves
  it must change.
- Exceptions, each isolated behind a library boundary:
  - Jolt uses its internal job system *inside* `PhysicsSystem::Update` only.
  - miniaudio mixes on its own audio thread; we communicate via its
    thread-safe API only.
- Any future cross-thread communication uses explicit queues, never shared
  mutable state.

## Error handling

- **Programmer bugs** → `ENG_ASSERT` (Debug-only, aborts). Never for runtime
  errors, never with side effects in the condition.
- **Expected runtime failures** (file missing, packet malformed, connection
  lost) → return values: `std::optional`, `std::expected<T, Error>`, or
  status enums. No exceptions in engine or game code (libraries may throw
  internally; we catch at the boundary if they do).
- **Untrusted input** (anything from the network) is validated at the
  deserialization boundary and rejected with a log line, never asserted.

## Comments and docs

- Comments state invariants, units, and coordinate conventions — not what
  the next line does.
- Every non-obvious architectural choice gets an ADR in `docs/decisions/`.
- Units and conventions: meters, seconds, radians internally; degrees only
  at UI/config boundaries; right-handed, +Y up, -Z forward (glTF/OpenGL).

## Testing

- Deterministic logic (serialization, sequence math, snapshot buffers,
  prediction, weapon cooldowns, damage rules) gets unit tests as it is
  written, not later.
- Tests live in `tests/`, mirroring the source tree
  (`tests/core/log_tests.cpp` tests `engine/core/log.*`).
- Rendering/audio/platform code is verified with manual checklists per
  milestone instead of unit tests.
