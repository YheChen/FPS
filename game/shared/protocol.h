#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/net/byte_buffer.h"
#include "game/shared/input_command.h"

// Wire protocol (docs/packet-format.md is the human-readable spec; keep in
// sync). Every message begins with a MessageType byte. Deserializers
// validate everything: lengths, ranges, float finiteness. A deserializer
// returning nullopt means "hostile or corrupt packet - drop it".
namespace game {

inline constexpr std::uint16_t kProtocolVersion = 2;
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
};

inline constexpr std::uint8_t kNoPlayer = 255;  // "no player" id (world/none)

// Snapshot player flags.
inline constexpr std::uint8_t kFlagOnGround = 1u << 0;
inline constexpr std::uint8_t kFlagAlive = 1u << 1;

enum class RejectReason : std::uint8_t {
    VersionMismatch = 1,
    ServerFull = 2,
    BadName = 3,
};

struct ClientHello {
    std::string name;  // 1..16 bytes
};

struct ServerWelcome {
    std::uint8_t player_id = 0;
    std::uint8_t tick_rate = 60;
    std::uint8_t snapshot_rate = 20;
    std::uint32_t server_tick = 0;
    std::string map;  // e.g. "maps/arena01.glb"
};

struct ServerReject {
    RejectReason reason = RejectReason::VersionMismatch;
};

struct PlayerJoined {
    std::uint8_t player_id = 0;
    std::string name;
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

// A shot was fired (visuals/audio on every client). hit_player == kNoPlayer
// means the shot hit world geometry or nothing.
struct FireEventMsg {
    std::uint8_t shooter = 0;
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    std::uint8_t hit_player = kNoPlayer;
};

struct PlayerDamagedMsg {
    std::uint8_t victim = 0;
    std::uint8_t attacker = 0;
    float health = 0.0f;  // victim's health after the hit
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
};

// Sent only to the owning player when their ammo/reload state changes.
struct WeaponStatusMsg {
    std::uint8_t ammo = 0;
    bool reloading = false;
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
void write(eng::ByteWriter& w, const WeaponStatusMsg& m);

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
std::optional<WeaponStatusMsg> read_weapon_status(eng::ByteReader& r);

// Reads and validates the leading type byte.
std::optional<MessageType> read_message_type(eng::ByteReader& r);

}  // namespace game
