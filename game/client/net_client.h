#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "engine/net/transport.h"
#include "game/shared/input_command.h"
#include "game/shared/interpolation.h"
#include "game/shared/protocol.h"

// Client-side connection state machine (networking stage 2: remote players
// carry snapshot-interpolation history; the local player's authoritative
// acks are surfaced for prediction reconciliation).
namespace game {

struct NetPlayer {
    std::string name;
    glm::vec3 position{0.0f};  // newest raw snapshot state
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool on_ground = false;
    bool seen_in_snapshot = false;
    SnapshotBuffer history;  // for interpolation (remote players)
};

// Authoritative state for the LOCAL player from the newest snapshot; input
// to Prediction::reconcile.
struct SelfAck {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool on_ground = false;
    std::uint32_t last_processed_input = 0;
    std::uint32_t server_tick = 0;
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
    // `view_tick` is the interpolation render tick (for server-side rewind).
    void send_input(const std::deque<InputCommand>& recent_newest_first,
                    std::uint32_t client_tick, std::uint32_t view_tick);

    State state() const { return state_; }
    const char* state_name() const;
    std::uint8_t my_id() const { return my_id_; }
    std::uint32_t server_tick() const { return latest_server_tick_; }
    std::uint32_t last_processed_input() const { return last_processed_input_; }
    std::uint32_t rtt_ms() const { return net_.rtt_ms(server_peer_); }
    const eng::NetStats& stats() const { return net_.stats(); }
    void set_simulation(const eng::NetSimConfig& config) { net_.set_simulation(config); }

    // All players by id, including the local one.
    const std::map<std::uint8_t, NetPlayer>& players() const { return players_; }

    // Consumes the newest authoritative self state (nullopt if none arrived
    // since the last call).
    std::optional<SelfAck> take_self_ack();

    // --- combat state (server-authoritative) ---------------------------
    struct Scores {
        std::uint16_t kills = 0;
        std::uint16_t deaths = 0;
    };
    const std::map<std::uint8_t, Scores>& scores() const { return scores_; }
    float self_health() const { return self_health_; }
    std::uint8_t self_ammo() const { return self_ammo_; }
    bool self_reloading() const { return self_reloading_; }
    bool self_alive() const { return self_alive_; }
    MatchPhase match_phase() const { return match_phase_; }
    std::uint16_t match_seconds() const { return match_seconds_; }

    // Drained event queues (visuals/audio are the caller's job).
    std::vector<FireEventMsg> take_fire_events();
    std::vector<PlayerDiedMsg> take_death_events();
    std::vector<PlayerRespawnedMsg> take_respawn_events();

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
    std::optional<SelfAck> pending_self_ack_;

    std::map<std::uint8_t, Scores> scores_;
    float self_health_ = 100.0f;
    std::uint8_t self_ammo_ = 0;
    bool self_reloading_ = false;
    bool self_alive_ = true;
    MatchPhase match_phase_ = MatchPhase::Playing;
    std::uint16_t match_seconds_ = 0;
    std::vector<FireEventMsg> fire_events_;
    std::vector<PlayerDiedMsg> death_events_;
    std::vector<PlayerRespawnedMsg> respawn_events_;
};

}  // namespace game
