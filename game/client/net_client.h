#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "engine/net/transport.h"
#include "game/shared/input_command.h"
#include "game/shared/protocol.h"

// Client-side connection state machine (networking stage 1: raw snapshot
// application, no prediction or interpolation yet - that is Milestone 7).
namespace game {

struct NetPlayer {
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool on_ground = false;
    bool seen_in_snapshot = false;
};

class NetClient {
public:
    enum class State : std::uint8_t {
        Connecting,       // ENet handshake in flight
        AwaitingWelcome,  // hello sent
        InGame,
        Rejected,
        Disconnected,
    };

    static std::optional<NetClient> connect(const std::string& host, std::uint16_t port,
                                            std::string player_name);

    // Pumps the network; updates players from the newest snapshot.
    void poll();

    // Sends the newest command plus up to 2 previous ones (loss redundancy).
    void send_input(const std::deque<InputCommand>& recent_newest_first,
                    std::uint32_t client_tick);

    State state() const { return state_; }
    const char* state_name() const;
    std::uint8_t my_id() const { return my_id_; }
    std::uint32_t server_tick() const { return latest_server_tick_; }
    std::uint32_t last_processed_input() const { return last_processed_input_; }
    std::uint32_t rtt_ms() const { return net_.rtt_ms(server_peer_); }
    const eng::NetStats& stats() const { return net_.stats(); }

    // All players by id, including the local one.
    const std::map<std::uint8_t, NetPlayer>& players() const { return players_; }

private:
    explicit NetClient(eng::NetHost net) : net_(std::move(net)) {}

    void handle_message(const std::vector<std::uint8_t>& data);

    eng::NetHost net_;
    std::uint32_t server_peer_ = 0;
    std::string player_name_;
    State state_ = State::Connecting;
    std::uint8_t my_id_ = 0;
    std::uint32_t latest_server_tick_ = 0;
    std::uint32_t last_processed_input_ = 0;
    std::map<std::uint8_t, NetPlayer> players_;
};

}  // namespace game
