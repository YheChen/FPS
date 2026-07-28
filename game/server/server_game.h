#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/net/transport.h"
#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
#include "game/shared/bot.h"
#include "game/shared/health.h"
#include "game/shared/input_command.h"
#include "game/shared/lag_comp.h"
#include "game/shared/player_movement.h"
#include "game/shared/protocol.h"
#include "game/shared/replay.h"
#include "game/shared/weapon.h"

// Authoritative game server: owns the physics world and ALL game state
// (positions, health, weapons, deaths, scores, match timer); consumes
// validated client inputs at a fixed 60 Hz tick; broadcasts 20 Hz full
// snapshots plus reliable combat events. Clients never send positions or
// hit claims - fire is a button in InputCommand, resolved and validated
// entirely server-side (rate, ammo, reload, range, alive checks).
namespace game {

inline constexpr float kMatchSeconds = 300.0f;
inline constexpr float kRespawnSeconds = 3.0f;
inline constexpr float kMatchRestartSeconds = 8.0f;

class ServerGame {
public:
    // `map_meshes` are (mesh, world transform) pairs for static collision;
    // `spawns` are the map's spawn points.
    ServerGame(std::vector<std::pair<eng::MeshData, glm::mat4>> map_meshes,
               std::vector<glm::vec3> spawns, std::string map_name, Arsenal arsenal);

    // Handles one transport event (connect/message/disconnect).
    void handle_event(const eng::NetEvent& event, eng::IServerTransport& net);

    // Advances one fixed tick and sends snapshots when due.
    void tick(eng::IServerTransport& net);

    std::uint32_t current_tick() const { return tick_; }

    // Starts recording every input the simulation actually consumes. Must be
    // called before the first tick; the file is written by write_replay().
    // Adds a bot in a normal player slot. Returns false when the server is
    // full. Bots are ordinary players whose commands are synthesized, so
    // snapshots, hit detection, scoring and replay recording need no special
    // cases for them.
    bool add_bot(std::string name);

    void start_recording(std::filesystem::path path);
    bool recording() const { return recorder_.recording(); }
    // Writes the recording started by start_recording(). No-op (returning
    // true) when not recording.
    bool write_replay() const;
    std::size_t player_count() const;

private:
    struct Player {
        std::uint32_t peer = 0;
        std::string name;
        // A bot has no peer: nothing is ever sent to it, and its commands
        // come from decide() instead of the network.
        bool is_bot = false;
        BotState bot_state;
        PlayerState state;
        std::unique_ptr<eng::CharacterController> controller;
        float view_yaw = 0.0f;
        float view_pitch = 0.0f;

        // Combat (authoritative).
        Health health;
        Loadout loadout;
        bool alive = true;
        float respawn_remaining = 0.0f;
        std::uint16_t kills = 0;
        std::uint16_t deaths = 0;

        std::uint32_t last_processed_input = 0;
        std::map<std::uint32_t, InputCommand> pending;  // seq -> command
        InputCommand last_command;                      // reused when starved
        int starved_ticks = 0;

        // Lag compensation: recent positions + the client's claimed view
        // tick (already range-limited at deserialization, clamped on use).
        PositionHistory history;
        std::uint32_t view_tick = 0;

        // Basic abuse accounting.
        int input_packets_this_second = 0;
        int bad_messages = 0;
    };

    void handle_hello(std::uint32_t peer, eng::ByteReader& reader, eng::IServerTransport& net);
    void handle_input(Player& player, eng::ByteReader& reader, eng::IServerTransport& net);
    // Gathers what a bot can perceive. Lives here, not in decide(), so the
    // decision layer stays a pure function with no world access.
    BotSenses sense_for_bot(std::uint8_t bot_id) const;
    void drop_player(std::uint8_t player_id, eng::IServerTransport& net);
    std::optional<std::uint8_t> find_player_by_peer(std::uint32_t peer) const;
    void send_snapshots(eng::IServerTransport& net);

    // Combat.
    void fire_hitscan(std::uint8_t shooter_id, const InputCommand& command,
                      eng::IServerTransport& net);
    void kill_player(std::uint8_t victim_id, std::uint8_t killer_id, eng::IServerTransport& net);
    void respawn_player(std::uint8_t player_id, eng::IServerTransport& net);
    void update_match(eng::IServerTransport& net);
    void restart_match(eng::IServerTransport& net);
    void broadcast_reliable(const std::vector<std::uint8_t>& data, eng::IServerTransport& net);
    void send_weapon_status(const Player& player, eng::IServerTransport& net);
    MatchStateMsg match_state() const;

    eng::PhysicsWorld world_;
    std::vector<glm::vec3> spawns_;
    std::string map_name_;
    Arsenal arsenal_;
    std::array<std::optional<Player>, kMaxPlayers> players_;
    BotConfig bot_config_;
    ReplayRecorder recorder_;
    std::filesystem::path replay_path_;
    std::uint32_t tick_ = 0;
    std::size_t next_spawn_ = 0;

    MatchPhase phase_ = MatchPhase::Playing;
    float match_remaining_ = kMatchSeconds;
    float restart_remaining_ = 0.0f;
};

}  // namespace game
