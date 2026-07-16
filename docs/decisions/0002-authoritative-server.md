# ADR 0002 — Authoritative server with client prediction

Status: accepted (2026-07-15)

## Context

Multiplayer FPS state must stay consistent and resistant to trivial cheating,
while local movement must feel instant despite latency.

## Decision

- The dedicated server owns all game state and is the only writer of it.
  Clients send **inputs**, never positions or hit claims.
- Fixed 60 Hz simulation tick on server and client; 20 Hz full-state
  snapshots; client-side prediction with reconciliation; snapshot
  interpolation (~100 ms) for remote players; server-side lag compensation
  for hitscan in Milestone 9.
- The player simulation code is compiled into both binaries from
  `game/shared/` so prediction replays the authoritative code exactly.
- Networking is built in stages (see docs/networking.md) but the
  architecture above is fixed from Milestone 6 onward — no "add prediction
  later" retrofits of a client-authoritative design.

## Consequences

- Structurally immune to speed/teleport/forged-hit cheating; NOT anti-cheat
  (aimbots and wallhacks remain possible).
- Shared-code discipline: any movement/weapon logic touching gameplay
  outcomes must live in game/shared and stay renderer-free.
- Costs: reconciliation complexity, an interpolation delay for remote
  players, and server CPU for rewind buffers in M9. All standard, all
  accepted.
