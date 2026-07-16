# Networking

## Model

Authoritative client-server. The dedicated server owns all game state:
positions, health, damage, deaths, respawns, score, match timer, and weapon
validation. Clients send timestamped **inputs only** and receive **snapshots**
of authoritative state.

Transport: **ENet** (UDP with optional per-channel reliability/ordering).
- Channel 0 — reliable, ordered: handshake, join/leave, game events, scoreboard.
- Channel 1 — unreliable, sequenced: input commands, snapshots. Stale packets
  are dropped by ENet's sequencing; we additionally check our own tick/sequence
  numbers.

## Rates

| Rate                  | Value            | Rationale |
|-----------------------|------------------|-----------|
| Server simulation     | **60 Hz fixed**  | Hitscan FPS feel; 8 players on one small map is cheap to simulate; matches client tick so prediction replays 1:1. Config constant — drop to 30 Hz only if profiling forces it. |
| Client simulation     | **60 Hz fixed**  | Same tick length and same shared movement code as the server; required for cheap, exact reconciliation replay. |
| Client input send     | 60 packets/s     | One packet per tick; each packet redundantly carries the last 3 input commands so a single lost packet costs nothing. |
| Server snapshots      | **20 Hz**        | Every 3rd tick. 8 players × ~30 B ≈ 240 B + header, ×20/s ≈ ~6 kB/s per client — comfortable. Interpolation hides the gaps. |
| Render frame rate     | Uncapped / vsync | Fully decoupled. Remote entities interpolate ~100 ms in the past (2 snapshot intervals + jitter margin); the local player renders from predicted state. |

### How the rates interact

- The client stamps every `InputCommand` with a **sequence number** and its
  **predicted server tick**, applies it locally at once (prediction), stores it
  in a ring buffer, and sends it.
- The server buffers inputs per player (small de-jitter buffer), consumes
  exactly one per tick, validates it, and simulates.
- Each snapshot includes `last_processed_input_seq` for the recipient. The
  client discards acknowledged history, re-applies the still-unacknowledged
  inputs on top of the server state, and smooths any residual error over a few
  frames instead of snapping.
- Remote players are never predicted; they are interpolated between the two
  snapshots that bracket `render_time = newest_snapshot_time - interp_delay`.

## Stages

1. **Connect & move (M6):** connect, assign player ID, send inputs, server
   simulates, broadcast transforms, remote players visible. Movement will look
   rough — that is expected.
2. **Responsive movement (M7):** sequence numbers, tick sync, snapshot
   interpolation, client prediction, server reconciliation, artificial
   latency/loss simulation for testing.
3. **Combat (M8):** hitscan fire through validated inputs, server-side
   fire-rate/ammo checks, damage, death, respawn, scoreboard, match timer.
4. **Lag compensation & optimization (M9):** server-side rewind of player
   hitboxes for hitscan (bounded to ~200 ms), bandwidth measurement, then —
   only if profiling demands — quantization/delta compression.

## Hostile-input assumptions

Every message is untrusted. The deserializer validates length, type, enum
ranges, string lengths, and float sanity (NaN/Inf rejected). The server
additionally enforces:

- max packet size and per-connection message rate (excess → drop, then kick);
- monotonically increasing input sequence numbers; duplicates and stale inputs
  ignored;
- input contents: pitch clamped to ±89°, movement axes clamped to [-1, 1];
- movement is *simulated server-side from inputs*, so speed/teleport cheats are
  structurally impossible rather than heuristically detected;
- fire rate, ammo, reload timing, and range checked against server state;
- timeouts: no packets for 5 s → disconnect.

This is **basic server authority, not anti-cheat**. It stops packet forgery
and impossible actions. It does not stop aimbots, wallhacks (clients receive
full state), or timing exploits within tolerance windows. Production
anti-cheat is out of scope and should be stated as such anywhere this project
is described.

## Failure handling

- **Loss:** inputs are sent redundantly; snapshots are full-state, so a lost
  snapshot merely widens interpolation. Reliable channel handles events.
- **Duplication/reordering:** sequence numbers on inputs; snapshot tick must
  exceed the last accepted one or the packet is dropped.
- **Disconnects:** ENet disconnect events + our timeout; player entity removed
  and `PlayerLeft` broadcast.
- **Malformed packets:** deserialization returns an error; packet dropped and
  counted; repeated garbage → kick.
