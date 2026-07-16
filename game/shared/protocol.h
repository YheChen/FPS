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

inline constexpr std::uint16_t kProtocolVersion = 1;
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
};

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

// --- encode ---------------------------------------------------------------
void write(eng::ByteWriter& w, const ClientHello& m);
void write(eng::ByteWriter& w, const ServerWelcome& m);
void write(eng::ByteWriter& w, const ServerReject& m);
void write(eng::ByteWriter& w, const PlayerJoined& m);
void write(eng::ByteWriter& w, const PlayerLeft& m);
void write(eng::ByteWriter& w, const InputPacket& m);
void write(eng::ByteWriter& w, const Snapshot& m);

// --- decode (after the type byte has been consumed) -------------------------
std::optional<ClientHello> read_client_hello(eng::ByteReader& r);
std::optional<ServerWelcome> read_server_welcome(eng::ByteReader& r);
std::optional<ServerReject> read_server_reject(eng::ByteReader& r);
std::optional<PlayerJoined> read_player_joined(eng::ByteReader& r);
std::optional<PlayerLeft> read_player_left(eng::ByteReader& r);
std::optional<InputPacket> read_input_packet(eng::ByteReader& r);
std::optional<Snapshot> read_snapshot(eng::ByteReader& r);

// Reads and validates the leading type byte.
std::optional<MessageType> read_message_type(eng::ByteReader& r);

}  // namespace game
