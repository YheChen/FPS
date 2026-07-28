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
4. **Lag compensation (M9, done):** each input packet carries the client's
   interpolation `view_tick`; the server records per-player position
   history and validates hitscan against victims where the shooter saw
   them, clamped to a 15-tick (~250 ms) rewind window. Quantization/delta
   compression remain unimplemented by choice: measured bandwidth at 8
   players (~5 kB/s per client) does not justify them.

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

## WebRTC DataChannel transport (M19a)

Browsers cannot open UDP sockets, so the browser client reaches the server
over WebSockets — which is TCP, and therefore head-of-line blocks. One lost
packet stalls every snapshot queued behind it, which is exactly the failure a
60 Hz shooter cannot absorb. A DataChannel configured **unordered with no
retransmits** is the UDP semantics a browser will actually give us.

`eng::WebRtcHost` implements `IServerTransport`, so it drops into
`CompositeTransport` beside ENet and WebSocket with no changes to
`ServerGame`. Two DataChannels per peer mirror the ENet channel split:

| Channel | Semantics | Carries |
|---|---|---|
| `reliable` | reliable, ordered | handshake, events |
| `sequenced` | **unordered, `maxRetransmits: 0`** | inputs, snapshots |

Leaving `sequenced` ordered would reproduce the exact blocking this transport
exists to remove.

### Signalling is not built in

`WebRtcHost` produces and consumes SDP and ICE candidates as opaque strings
(`accept_offer`, `add_remote_candidate`, `take_signals`). The caller carries
them over whatever channel it likes — the existing WebSocket transport, in the
game's case. That keeps a second listening socket out of this class and makes
the transport testable in-process.

### Threading

libdatachannel fires callbacks on its own threads. Everything they touch is
behind one mutex and drained on the main thread in `poll()`, so nothing
outside this file ever sees another thread.

**The trap:** destroying an `rtc::PeerConnection` joins its callback threads,
and those callbacks take that same mutex. Reaping a closed peer while holding
the lock deadlocks — `poll()` waits for the thread, the thread waits for the
mutex. Doomed peers are moved out under the lock and destroyed after it is
released. This hung the test suite indefinitely before it was found.

### Opt-in, and why

`FPS_ENABLE_WEBRTC` defaults **OFF**. libdatachannel pulls five submodules and
needs a TLS backend, and Windows runners have no system OpenSSL — enabling it
by default would break the three-platform CI for a transport most builds do
not need. A dedicated Ubuntu CI job builds and tests the enabled path, so it
is not left to a developer's machine.

Binary messages only. A browser's `dataChannel.send(ArrayBuffer)` is binary;
a text message means something that is not our client is talking to us.
