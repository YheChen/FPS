# Packet format

Status: **design** — implementation begins in Milestone 6. This document is
the source of truth; update it in the same commit as any protocol change.

## Encoding rules

- All multi-byte integers are **little-endian**.
- Floats are IEEE-754 binary32 (little-endian byte order).
- Strings: `u8 length` followed by UTF-8 bytes, no terminator. Max lengths
  are per-field and enforced on read.
- Serialization goes through `eng::net::ByteWriter` / `ByteReader` (M6).
  `ByteReader` never reads past the end: every accessor returns
  `std::expected`/optional-style failure, and one failure poisons the reader.
- **Never** `memcpy` a struct to the wire. No padding, no host byte order,
  no implicit layout on the wire, ever.
- Every packet begins with `u8 message_type`. Unknown type → drop packet,
  count it, and after repeated abuse, kick.
- `protocol_version` (u16) is checked in the handshake; mismatch → reject.

## Message catalog (initial, stages 1–3)

Channel R = ENet reliable ordered (ch 0), U = unreliable sequenced (ch 1).

| Type | Name             | Dir  | Ch | Frequency        | Max size | Notes |
|------|------------------|------|----|------------------|----------|-------|
| 1    | ClientHello      | C→S  | R  | once             | 24 B     | version + name |
| 2    | ServerWelcome    | S→C  | R  | once             | 48 B     | id, rates, tick, map |
| 3    | ServerReject     | S→C  | R  | once             | 4 B      | reason code |
| 4    | PlayerJoined     | S→C  | R  | on join          | 24 B     | |
| 5    | PlayerLeft       | S→C  | R  | on leave         | 4 B      | |
| 6    | InputCommand     | C→S  | U  | 60/s             | 64 B     | redundant window of 3 |
| 7    | Snapshot         | S→C  | U  | 20/s             | 16 + 26·players B | full state |
| 8    | Ping / 9 Pong    | both | U  | 4/s              | 12 B     | RTT + clock offset |
| 10   | FireEvent        | S→C  | R  | on shot (M8)     | 32 B     | shooter, hit point, victim |
| 11   | HealthUpdate     | S→C  | R  | on change (M8)   | 8 B      | |
| 12   | PlayerDied       | S→C  | R  | on death (M8)    | 8 B      | victim, killer |
| 13   | PlayerRespawned  | S→C  | R  | on respawn (M8)  | 20 B     | |
| 14   | ScoreUpdate      | S→C  | R  | on change (M8)   | 8 B/row  | |
| 15   | MatchState       | S→C  | R  | on change + join | 16 B     | phase, time remaining |

### ClientHello (C→S, reliable, once)

| Field            | Type   | Validation |
|------------------|--------|------------|
| protocol_version | u16    | must equal server's, else ServerReject(version) |
| player_name      | string | 1–16 bytes after trim; printable UTF-8; else reject |

### ServerWelcome (S→C, reliable, once)

| Field            | Type | Notes |
|------------------|------|-------|
| player_id        | u8   | 0–7 |
| tick_rate        | u8   | 60 |
| snapshot_rate    | u8   | 20 |
| server_tick      | u32  | client seeds its tick estimate from this |
| map_name         | string ≤ 32 | |

### InputCommand (C→S, unreliable, 60/s)

Carries the newest command plus the previous 2 (loss redundancy):

| Field                | Type | Validation |
|----------------------|------|------------|
| newest_sequence      | u32  | must be > last processed; window-limited (≤ last+64) |
| client_tick          | u32  | sanity vs. server tick estimate |
| count                | u8   | 1–3 |
| per command:         |      | |
| · buttons            | u8   | bitfield: fwd, back, left, right, jump, fire, reload, sprint |
| · yaw                | f32  | finite, wrapped to [-π, π) |
| · pitch              | f32  | finite, clamped to ±89° |

Rate limit: > 120 input packets/s sustained → warn, then kick.

### Snapshot (S→C, unreliable, 20/s)

| Field                     | Type | Notes |
|---------------------------|------|-------|
| server_tick               | u32  | must be > last accepted, else drop |
| last_processed_input_seq  | u32  | for the recipient; drives reconciliation |
| player_count              | u8   | ≤ 8 |
| per player:               |      | |
| · player_id               | u8   | |
| · position                | 3×f32| |
| · yaw, pitch              | 2×f32| |
| · state flags             | u8   | alive, crouched(later), firing (for remote FX) |

Full-state snapshots (no deltas) until profiling shows bandwidth pressure;
at 8 players ≈ 226 B × 20/s ≈ 4.5 kB/s per client.

## Versioning

Any wire change bumps `protocol_version`. No in-protocol backward
compatibility — both binaries ship together.
