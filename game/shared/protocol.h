#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/net/byte_buffer.h"
#include "game/shared/hitscan.h"
#include "game/shared/input_command.h"
#include "game/shared/kill_cam.h"
#include "game/shared/weapon.h"

// Wire protocol (docs/packet-format.md is the human-readable spec; keep in
// sync). Every message begins with a MessageType byte. Deserializers
// validate everything: lengths, ranges, float finiteness. A deserializer
// returning nullopt means "hostile or corrupt packet - drop it".
namespace game {

inline constexpr std::uint16_t kProtocolVersion = 11;
inline constexpr std::uint8_t kMaxPlayers = 8;
inline constexpr std::size_t kMaxNameLength = 16;
inline constexpr int kSnapshotDivisor = 3;  // 60 Hz ticks -> 20 Hz snapshots
inline constexpr std::size_t kInputRedundancy = 3;
inline constexpr std::uint32_t kMaxInputAhead = 64;  // seq window

enum class MessageType : std::uint8_t {
    ClientHello = 1,
    ServerWelcome = 2,
    ServerReject = 3,
    PlayerJoined = 4,
    PlayerLeft = 5,
    Input = 6,
    Snapshot = 7,
    // Combat (stage 3), all server -> client, reliable:
    FireEvent = 8,
    PlayerDamaged = 9,
    PlayerDied = 10,
    PlayerRespawned = 11,
    ScoreUpdate = 12,
    MatchState = 13,
    WeaponStatus = 14,
    // Career stats that outlive the match (M29), server -> client,
    // reliable. Pushed after the welcome and again at match end; the
    // client never asks for it, so there is no request to rate-limit.
    Leaderboard = 15,
    // Killcam (M30), server -> the victim only, reliable, on death.
    // Unicast rather than broadcast: it is the one message in the
    // protocol addressed to a single player about their own death.
    KillCam = 16,
    // WebRTC signalling (M19b). These ride the WebSocket connection, which
    // for a WebRTC client exists only to negotiate: once the DataChannel
    // opens, every byte of game traffic moves to it and nothing is sent
    // over the socket again.
    //
    // They are the only messages in the protocol that the game layer never
    // sees. A signalling router in front of ServerGame consumes them,
    // because ServerGame has no business knowing which transports exist --
    // and one reaching it would be counted as malformed and eventually kick
    // the client.
    RtcOffer = 17,      // client -> server, SDP
    RtcAnswer = 18,     // server -> client, SDP
    RtcCandidate = 19,  // both ways, ICE candidate + media id
    // Text chat (M50). ChatSend is the ONLY message whose payload a player
    // composes by hand, which makes it the only one where the content -- not
    // just the framing -- is hostile input.
    ChatSend = 20,     // client -> server, what the player typed
    ChatMessage = 21,  // server -> everyone, stamped with who actually sent it
    // Map rotation (M51), server -> everyone, reliable, at match end.
    //
    // The client loads geometry BEFORE it connects and refuses a server on a
    // different map, so this is the one message that asks it to rebuild the
    // world it is standing in rather than just to draw something new.
    MapChange = 22,
};

// An SDP session description runs to a couple of kilobytes -- by far the
// largest thing on this wire, and the reason long_str exists. A candidate
// line is short.
inline constexpr std::size_t kMaxSdpLength = 8192;
inline constexpr std::size_t kMaxCandidateLength = 512;
inline constexpr std::size_t kMaxMidLength = 64;

struct RtcOfferMsg {
    std::string sdp;
};

struct RtcAnswerMsg {
    std::string sdp;
};

struct RtcCandidateMsg {
    std::string candidate;
    std::string mid;  // which media section the candidate belongs to
};

// How many rows the server will ever send. Bounded on the wire because
// the store behind it is bounded too, and a leaderboard nobody can read
// in one screen is not a leaderboard.
inline constexpr std::size_t kLeaderboardSize = 10;

inline constexpr std::uint8_t kNoPlayer = 255;  // "no player" id (world/none)

// Long enough for a sentence, short enough that a full lobby spamming the cap
// is still a rounding error against one snapshot.
inline constexpr std::size_t kMaxChatLength = 120;

struct ChatSendMsg {
    std::string text;
};

// The sender is the SERVER's answer, never the client's: a player id the
// server filled in from the connection it arrived on. A client that could
// name its own sender could put words in another player's mouth.
struct ChatMessageMsg {
    std::uint8_t sender = kNoPlayer;
    std::string text;
};

// Strips what must never reach a renderer or a log, and truncates to the cap.
// Returns an empty string when nothing printable survives -- the caller drops
// those rather than broadcasting a blank line.
//
// Control characters are removed rather than escaped: a newline would let one
// message forge several lines in the chat log (and in the server's log), and
// an ESC could rewrite a terminal that is tailing journalctl. Truncation is
// by BYTES and deliberately does not split a UTF-8 sequence.
std::string sanitize_chat(std::string_view text);

// The map the next match will be played on. Sent to everyone at the moment
// the rotation advances, before any snapshot describing positions in it: a
// client that stepped a player through the OLD collision using the NEW
// server positions would fall through the floor.
struct MapChangeMsg {
    std::string map;  // e.g. "maps/arena02.glb"
};

// Which side a player is on (M52). TWO teams, and deliberately not N: two is
// what a spare snapshot bit buys, what the arena's spawn layout supports, and
// what "team deathmatch" means. A third team would be a different mode with a
// different scoreboard, not a bigger enum.
enum class Team : std::uint8_t { A = 0, B = 1 };

constexpr Team other_team(Team team) {
    return team == Team::A ? Team::B : Team::A;
}
constexpr const char* team_name(Team team) {
    return team == Team::A ? "A" : "B";
}

// Snapshot player flags.
inline constexpr std::uint8_t kFlagOnGround = 1u << 0;
inline constexpr std::uint8_t kFlagAlive = 1u << 1;
inline constexpr std::uint8_t kFlagCrouching = 1u << 2;
// Team B when set, team A when clear. Team rides EVERY snapshot rather than
// only the join message, because a player who missed a PlayerJoined -- or who
// was rebalanced between matches -- would otherwise be shooting at the wrong
// colour until they reconnected. One bit, and it is always current.
inline constexpr std::uint8_t kFlagTeamB = 1u << 3;

enum class RejectReason : std::uint8_t {
    VersionMismatch = 1,
    ServerFull = 2,
    BadName = 3,
};

// For logs, and for the one refusal a player is actually shown. "reason 1" in
// a log is a number someone has to go look up.
constexpr const char* reject_reason_name(RejectReason reason) {
    switch (reason) {
        case RejectReason::VersionMismatch:
            return "version mismatch";
        case RejectReason::ServerFull:
            return "server full";
        case RejectReason::BadName:
            return "bad name";
    }
    return "unknown";
}

struct ClientHello {
    std::string name;  // 1..16 bytes
};

struct ServerWelcome {
    std::uint8_t player_id = 0;
    std::uint8_t tick_rate = 60;
    std::uint8_t snapshot_rate = 20;
    std::uint32_t server_tick = 0;
    std::string map;  // e.g. "maps/arena02.glb"
    Team team = Team::A;
};

// One career record. These are NOT authenticated: there are no accounts,
// players are identified by the name they typed, and anyone may type any
// name. The client says so on screen -- a leaderboard that looks
// authoritative and is not would be worse than having none.
struct LeaderboardEntry {
    std::string name;
    std::uint32_t kills = 0;
    std::uint32_t deaths = 0;
    std::uint32_t matches = 0;
};

struct LeaderboardMsg {
    std::vector<LeaderboardEntry> entries;  // best first, at most kLeaderboardSize
};

// The seconds before a death, from the killer's eyes. Samples are oldest
// first at the snapshot rate. Empty when the killer is gone or the death had
// no killer (a fall, or the world), in which case the client just shows the
// ordinary death overlay.
struct KillCamMsg {
    std::uint8_t killer = kNoPlayer;
    std::vector<ViewSample> samples;
};

struct ServerReject {
    RejectReason reason = RejectReason::VersionMismatch;
};

struct PlayerJoined {
    std::uint8_t player_id = 0;
    std::string name;
    Team team = Team::A;
};

struct PlayerLeft {
    std::uint8_t player_id = 0;
};

// Carries the newest command plus up to kInputRedundancy-1 previous ones
// (loss redundancy). commands[0] is the OLDEST; sequence numbers are
// consecutive ending at newest_sequence.
struct InputPacket {
    std::uint32_t newest_sequence = 0;
    std::uint32_t client_tick = 0;
    // The server tick the client was RENDERING remote players at when these
    // commands were made (its interpolation render tick). Drives server-side
    // rewind for hitscan lag compensation; clamped by the server to
    // [current - kMaxRewindTicks, current]. 0 = no estimate yet.
    std::uint32_t view_tick = 0;
    std::vector<InputCommand> commands;  // 1..kInputRedundancy, seq filled on read
};

struct SnapshotPlayer {
    std::uint8_t player_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::uint8_t flags = 0;  // bit0: on_ground
};

struct Snapshot {
    std::uint32_t server_tick = 0;
    std::uint32_t last_processed_input = 0;  // per recipient
    std::vector<SnapshotPlayer> players;     // <= kMaxPlayers
};

// One ray of a shot. hit_player == kNoPlayer means it hit world geometry or
// nothing.
struct FireRay {
    glm::vec3 to{0.0f};
    std::uint8_t hit_player = kNoPlayer;
};

// A shot was fired (visuals/audio on every client). A single trigger pull is
// ONE event carrying every pellet's ray, so a shotgun draws 8 tracers but
// plays one bang.
struct FireEventMsg {
    std::uint8_t shooter = 0;
    std::uint8_t slot = 0;  // weapon that fired, for per-weapon sound/FX
    glm::vec3 from{0.0f};
    std::vector<FireRay> rays;  // 1..kMaxPellets
};

inline constexpr std::size_t kMaxPellets = 32;

struct PlayerDamagedMsg {
    std::uint8_t victim = 0;
    std::uint8_t attacker = 0;
    float health = 0.0f;  // victim's health after the hit
    float amount = 0.0f;  // damage dealt, for the attacker's damage numbers
    // Where it landed, so the shooter's hitmarker can say "head" and the kill
    // feed can too. For a shotgun this is the best zone any pellet reached,
    // not a per-pellet list: one trigger pull is one damage event.
    HitZone zone = HitZone::Torso;
};

struct PlayerDiedMsg {
    std::uint8_t victim = 0;
    std::uint8_t killer = 0;  // kNoPlayer for environment deaths
};

struct PlayerRespawnedMsg {
    std::uint8_t player = 0;
    glm::vec3 position{0.0f};
};

struct ScoreUpdateMsg {
    std::uint8_t player = 0;
    std::uint16_t kills = 0;
    std::uint16_t deaths = 0;
};

enum class MatchPhase : std::uint8_t {
    Playing = 1,
    Ended = 2,
};

struct MatchStateMsg {
    MatchPhase phase = MatchPhase::Playing;
    std::uint16_t seconds_remaining = 0;
    // Team kills. The per-player ScoreUpdate stays exactly as it was: these
    // are not a sum the client could compute for itself, because it only
    // knows about players currently connected and a team keeps the kills of
    // players who have since left.
    std::uint16_t score_a = 0;
    std::uint16_t score_b = 0;
};

// Sent only to the owning player when their ammo/reload state changes.
struct WeaponStatusMsg {
    std::uint8_t ammo = 0;
    bool reloading = false;
    std::uint8_t slot = 0;      // which weapon is raised
    std::uint8_t magazine = 0;  // that weapon's magazine size, for the HUD
    bool switching = false;     // weapon still being raised
};

// --- encode ---------------------------------------------------------------
void write(eng::ByteWriter& w, const ClientHello& m);
void write(eng::ByteWriter& w, const ServerWelcome& m);
void write(eng::ByteWriter& w, const ServerReject& m);
void write(eng::ByteWriter& w, const PlayerJoined& m);
void write(eng::ByteWriter& w, const PlayerLeft& m);
void write(eng::ByteWriter& w, const InputPacket& m);
void write(eng::ByteWriter& w, const Snapshot& m);
void write(eng::ByteWriter& w, const FireEventMsg& m);
void write(eng::ByteWriter& w, const PlayerDamagedMsg& m);
void write(eng::ByteWriter& w, const PlayerDiedMsg& m);
void write(eng::ByteWriter& w, const PlayerRespawnedMsg& m);
void write(eng::ByteWriter& w, const ScoreUpdateMsg& m);
void write(eng::ByteWriter& w, const MatchStateMsg& m);
void write(eng::ByteWriter& w, const LeaderboardMsg& m);
void write(eng::ByteWriter& w, const KillCamMsg& m);
void write(eng::ByteWriter& w, const WeaponStatusMsg& m);
void write(eng::ByteWriter& w, const RtcOfferMsg& m);
void write(eng::ByteWriter& w, const RtcAnswerMsg& m);
void write(eng::ByteWriter& w, const RtcCandidateMsg& m);
void write(eng::ByteWriter& w, const ChatSendMsg& m);
void write(eng::ByteWriter& w, const ChatMessageMsg& m);
void write(eng::ByteWriter& w, const MapChangeMsg& m);

// --- decode (after the type byte has been consumed) -------------------------
std::optional<ClientHello> read_client_hello(eng::ByteReader& r);
std::optional<ServerWelcome> read_server_welcome(eng::ByteReader& r);
std::optional<ServerReject> read_server_reject(eng::ByteReader& r);
std::optional<PlayerJoined> read_player_joined(eng::ByteReader& r);
std::optional<PlayerLeft> read_player_left(eng::ByteReader& r);
std::optional<InputPacket> read_input_packet(eng::ByteReader& r);
std::optional<Snapshot> read_snapshot(eng::ByteReader& r);
std::optional<FireEventMsg> read_fire_event(eng::ByteReader& r);
std::optional<PlayerDamagedMsg> read_player_damaged(eng::ByteReader& r);
std::optional<PlayerDiedMsg> read_player_died(eng::ByteReader& r);
std::optional<PlayerRespawnedMsg> read_player_respawned(eng::ByteReader& r);
std::optional<ScoreUpdateMsg> read_score_update(eng::ByteReader& r);
std::optional<MatchStateMsg> read_match_state(eng::ByteReader& r);
std::optional<LeaderboardMsg> read_leaderboard(eng::ByteReader& r);
std::optional<KillCamMsg> read_kill_cam(eng::ByteReader& r);
std::optional<WeaponStatusMsg> read_weapon_status(eng::ByteReader& r);
std::optional<RtcOfferMsg> read_rtc_offer(eng::ByteReader& r);
std::optional<RtcAnswerMsg> read_rtc_answer(eng::ByteReader& r);
std::optional<RtcCandidateMsg> read_rtc_candidate(eng::ByteReader& r);
std::optional<ChatSendMsg> read_chat_send(eng::ByteReader& r);
std::optional<ChatMessageMsg> read_chat_message(eng::ByteReader& r);
std::optional<MapChangeMsg> read_map_change(eng::ByteReader& r);

// Reads and validates the leading type byte.
std::optional<MessageType> read_message_type(eng::ByteReader& r);

}  // namespace game
