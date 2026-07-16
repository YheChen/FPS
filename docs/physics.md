# Physics

Status: design stub — implementation starts in Milestone 4.

## Approach

- **Jolt Physics** for static map collision, raycasts, and (rarely) dynamic
  bodies.
- The player is a **kinematic character controller** built on Jolt's
  `CharacterVirtual`, not a dynamic rigid body. FPS movement (instant
  acceleration, air control, exact jump heights) is game code that *uses*
  collision queries; a physics solver fighting for the body's velocity makes
  movement mushy and network prediction harder.
- The character update is a pure function in `game/shared/`:
  `(PlayerState, InputCommand, dt, PhysicsWorld) -> PlayerState`, stepped at
  the fixed 60 Hz tick on both client (prediction) and server (authority).

## Determinism stance

Jolt is deterministic for identical binaries/platforms. We do NOT rely on
cross-platform bit-exact determinism: the server is authoritative and client
prediction errors are corrected by reconciliation. What we DO require is
that the same build replaying the same inputs from the same state produces
the same result (needed for prediction replay) — this holds and is covered
by unit tests from Milestone 4 on.

## Threading

Jolt's internal job system runs only inside the world step, called from the
main thread. No physics access from other threads.
