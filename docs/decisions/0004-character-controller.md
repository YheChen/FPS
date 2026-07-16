# ADR 0004 — Kinematic character controller; movement code in game/shared

Status: accepted (2026-07-15) — implemented in Milestone 4

## Context

FPS movement must be crisp (instant acceleration, exact jump heights, air
control) and must be replayable for client-side prediction. Driving a
dynamic rigid body with forces makes feel and determinism both worse.

## Decision

- Player movement uses Jolt's `CharacterVirtual` (kinematic: we integrate
  velocity ourselves, Jolt resolves collisions/steps/slopes).
- The per-tick update is a pure function in `game/shared/`:
  `advance(PlayerState, InputCommand, dt, collision queries) -> PlayerState`,
  identical on client (prediction) and server (authority), fixed 60 Hz.

## Consequences

- Movement tuning is plain game code, not solver parameters.
- Prediction replay is exact on a given build (unit-tested).
- Players don't automatically push dynamic objects; if that's ever wanted,
  it's explicit gameplay code.
