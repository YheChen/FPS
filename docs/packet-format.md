# Packet format

Status: **implemented** (protocol version 11, `game/shared/protocol.*`).
Update this document in the same commit as any protocol change.

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

| Type | Name | Dir | Ch | Frequency | Max size | Notes |
|------|------|-----|----|-----------|----------|-------|
| 1  | ClientHello     | C→S | R | once | 20 B | protocol version + name |
| 2  | ServerWelcome   | S→C | R | once | 74 B | id, rates, tick, map, team |
| 3  | ServerReject    | S→C | R | once | 2 B | reason code |
| 4  | PlayerJoined    | S→C | R | on join | 20 B | id, name, team |
| 5  | PlayerLeft      | S→C | R | on leave | 2 B | |
| 6  | Input           | C→S | U | 60/s | 47 B | redundant window of 3 |
| 7  | Snapshot        | S→C | U | 20/s | 10 + 34·players B | full state |
| 8  | FireEvent       | S→C | R | on shot | 16 + 13·pellets B | shooter, weapon slot, origin, one ray (endpoint + victim) per pellet: a shotgun draws 8 tracers but plays one bang |
| 9  | PlayerDamaged   | S→C | R | on hit | 12 B | victim, attacker, health after, damage dealt, and **hit zone** (M35): 0 torso, 1 head, 2 arm, 3 leg. One shot reports one zone — for a shotgun, the best any pellet reached. A zone byte above 3 rejects the message. |
| 10 | PlayerDied      | S→C | R | on death | 3 B | victim, killer |
| 11 | PlayerRespawned | S→C | R | on respawn | 14 B | |
| 12 | ScoreUpdate     | S→C | R | on change | 6 B | per player |
| 13 | MatchState      | S→C | R | on change + join | 8 B | phase, time remaining, both team scores |
| 14 | WeaponStatus    | S→**owner** | R | on ammo/reload/slot change | 6 B | unicast: nobody else needs your magazine |
| 15 | Leaderboard     | S→C | R | on join + match end (M29) | 2 + 29·rows B, ≤ 10 rows | career kills/deaths/matches |
| 16 | KillCam         | S→**victim** | R | on death (M30) | 3 + 20·samples B, ≤ 40 samples | the killer's view, oldest first |
| 17 | RtcOffer        | C→S | R | once per WebRTC join (M19b) | 8195 B | SDP; consumed by the signalling router, never by ServerGame |
| 18 | RtcAnswer       | S→C | R | once per WebRTC join (M19b) | 8195 B | SDP |
| 19 | RtcCandidate    | both | R | during ICE (M19b) | 580 B | candidate + media id |
| 20 | ChatSend        | C→S | R | rate limited to 1 per 45 ticks (M50) | 122 B | the only message whose *contents* a player composes |
| 21 | ChatMessage     | S→C | R | on relay (M50) | 123 B | sender is the server's answer, not the client's claim |
| 22 | MapChange       | S→C | R | at match end, with rotation (M51) | 66 B | rebuild the world before the next snapshot describes it |

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
| map_name         | string ≤ 64 | the client refuses a server on a different map |
| team             | u8   | 0 = A, 1 = B; anything else rejects the message |

### InputCommand (C→S, unreliable, 60/s)

Carries the newest command plus the previous 2 (loss redundancy):

| Field                | Type | Validation |
|----------------------|------|------------|
| newest_sequence      | u32  | must be > last processed; window-limited (≤ last+64) |
| client_tick          | u32  | sanity vs. server tick estimate |
| view_tick            | u32  | the tick the client was RENDERING remote players at; drives server-side rewind, clamped to [current − 15, current] on use. 0 = no estimate yet |
| count                | u8   | 1–3 |
| per command (wire order): |  | |
| · yaw                | f32  | finite and \|yaw\| ≤ 8 rad — a wrapped angle plus slack; the reader rejects, it does not re-wrap |
| · pitch              | f32  | finite and within ±89° (`kMaxPitch`) plus 0.01 rad of slack |
| · buttons            | u16  | bitfield: fwd, back, left, right, jump, fire, reload, sprint, crouch, aim |
| · weapon_slot        | u8   | desired weapon, < `kMaxWeapons` (5); sent as state every tick so a lost packet cannot drop a switch |

Rate limit: > 200 input packets/s (`kMaxInputPacketsPerSecond`) drops the
connection on the packet that crosses it; the counter resets every 60 ticks.
Ten malformed messages (`kMaxBadMessages`) also kick.

### Snapshot (S→C, unreliable, 20/s)

| Field                     | Type | Notes |
|---------------------------|------|-------|
| server_tick               | u32  | must be > last accepted, else drop |
| last_processed_input_seq  | u32  | for the recipient; drives reconciliation |
| player_count              | u8   | ≤ 8 |
| per player:               |      | |
| · player_id               | u8   | |
| · position                | 3×f32| feet |
| · velocity                | 3×f32| lets the client extrapolate and keeps reconciliation exact |
| · yaw, pitch              | 2×f32| |
| · state flags             | u8   | bit0 on_ground, bit1 alive, bit2 crouching, bit3 team B (M52) |

Header is 10 B (type, server_tick, last_processed_input, player_count) and
each player is 34 B, so a full 8-player snapshot is 282 B. Full-state (no
deltas) until profiling shows bandwidth pressure: 282 B × 20/s ≈ 5.6 kB/s per
client on the wire.

## Versioning

Any wire change bumps `protocol_version`. No in-protocol backward
compatibility — both binaries ship together.

**"Ship together" is a deployment constraint, not just a slogan.** The client
is a static bundle on a CDN and the server is a container on someone's desk;
they are deployed by different mechanisms and nothing enforces the ordering.
A version bump that reaches only one of them turns every connection into
`ServerReject(VersionMismatch)`. Every bump since M29 has been in that position; the
newest is M52's 10 → 11, and it may not reach the live server before the
matching client is published.
`tools/server_autodeploy.sh` enforces exactly this and refuses to deploy
across a version gap.

There is one grace in the design: an unknown message *type* is rejected by
`read_message_type` rather than skipped, so a client that somehow saw a newer
message would drop it rather than misparse it. That is a safety net against
corruption, not a compatibility mechanism.

## WebRTC signalling (M19b): RtcOffer 17, RtcAnswer 18, RtcCandidate 19

The only messages the game layer never sees. They ride the WebSocket
connection; a router in front of `ServerGame` consumes them, because a
signalling message reaching the game would be counted as malformed and would
eventually kick the client.

| Field | Type | Notes |
|---|---|---|
| **RtcOffer** (client -> server) | | |
| sdp | long_str | u16 length prefix, <= 8192 |
| **RtcAnswer** (server -> client) | | |
| sdp | long_str | u16 length prefix, <= 8192 |
| **RtcCandidate** (both ways) | | |
| candidate | long_str | u16 length prefix, <= 512 |
| mid | str | u8 length prefix, <= 64 |

`long_str` is a u16-length-prefixed string, added for exactly this: an SDP is
a couple of kilobytes and `str()` caps at 255 bytes. Every other field on this
wire keeps its one-byte prefix, so a 300-byte player name stays impossible.

A hostile peer can claim 60 KB and send none; the reader rejects any length
past the caller's own limit rather than allocating on the claim.

## Text chat (M50): ChatSend 20, ChatMessage 21

| Field | Type | Notes |
|---|---|---|
| **ChatSend** (client -> server) | | |
| text | str | u8 length prefix, <= 120 bytes |
| **ChatMessage** (server -> everyone) | | |
| sender | u8 | < 8; the SERVER's answer, not the client's |
| text | str | u8 length prefix, <= 120 bytes, already sanitized |

`ChatSend` carries **no sender field**. There is nothing to forge: the server
stamps the id of the connection the message arrived on. A client that could
name its own sender could put words in another player's mouth.

Over-length is **rejected, not truncated** — `str(kMaxChatLength)` refuses a
length past the cap, so a hostile 200-byte message is dropped at the reader
rather than silently shortened.

`sanitize_chat()` then removes C0 controls and DEL. A newline is the one that
matters: it would let a single message forge a second line in the chat log
*and* in the server's own log, and an ESC could rewrite the terminal of
whoever is tailing `journalctl`. Bytes >= 0x80 are kept so accented names
survive, and truncation never splits a UTF-8 sequence.
