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
#include "game/shared/input_command.h"
#include "game/shared/player_movement.h"
#include "game/shared/protocol.h"

// Authoritative game server (networking stage 1): owns the physics world and
// all player state; consumes validated client inputs at a fixed 60 Hz tick;
// broadcasts 20 Hz full snapshots. Clients never send positions.
namespace game {

class ServerGame {
public:
    // `map_meshes` are (mesh, world transform) pairs for static collision;
    // `spawns` are the map's spawn points.
    ServerGame(std::vector<std::pair<eng::MeshData, glm::mat4>> map_meshes,
               std::vector<glm::vec3> spawns, std::string map_name);

    // Handles one transport event (connect/message/disconnect).
    void handle_event(const eng::NetEvent& event, eng::NetHost& net);

    // Advances one fixed tick and sends snapshots when due.
    void tick(eng::NetHost& net);

    std::uint32_t current_tick() const { return tick_; }
    std::size_t player_count() const;

private:
    struct Player {
        std::uint32_t peer = 0;
        std::string name;
        PlayerState state;
        std::unique_ptr<eng::CharacterController> controller;
        float view_yaw = 0.0f;
        float view_pitch = 0.0f;

        std::uint32_t last_processed_input = 0;
        std::map<std::uint32_t, InputCommand> pending;  // seq -> command
        InputCommand last_command;                      // reused when starved
        int starved_ticks = 0;

        // Basic abuse accounting.
        int input_packets_this_second = 0;
        int bad_messages = 0;
    };

    void handle_hello(std::uint32_t peer, eng::ByteReader& reader, eng::NetHost& net);
    void handle_input(Player& player, eng::ByteReader& reader, eng::NetHost& net);
    void drop_player(std::uint8_t player_id, eng::NetHost& net);
    std::optional<std::uint8_t> find_player_by_peer(std::uint32_t peer) const;
    void send_snapshots(eng::NetHost& net);

    eng::PhysicsWorld world_;
    std::vector<glm::vec3> spawns_;
    std::string map_name_;
    std::array<std::optional<Player>, kMaxPlayers> players_;
    std::uint32_t tick_ = 0;
    std::size_t next_spawn_ = 0;
};

}  // namespace game
