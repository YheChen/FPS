#include "game/client/net_client.h"

#include <algorithm>
#include <utility>

#include "engine/core/log.h"

namespace game {

std::optional<NetClient> NetClient::connect(const std::string& host, std::uint16_t port,
                                            std::string player_name) {
    auto net = eng::NetHost::create_client();
    if (!net) {
        return std::nullopt;
    }
    NetClient client{std::move(*net)};
    const auto peer = client.net_.connect(host, port);
    if (!peer) {
        return std::nullopt;
    }
    client.server_peer_ = *peer;
    client.player_name_ = std::move(player_name);
    if (client.player_name_.empty() || client.player_name_.size() > kMaxNameLength) {
        client.player_name_ = "player";
    }
    return client;
}

const char* NetClient::state_name() const {
    switch (state_) {
        case State::Connecting:
            return "connecting";
        case State::AwaitingWelcome:
            return "awaiting welcome";
        case State::InGame:
            return "in game";
        case State::Rejected:
            return "rejected";
        case State::Disconnected:
            return "disconnected";
    }
    return "?";
}

void NetClient::poll() {
    std::vector<eng::NetEvent> events;
    net_.poll(events);
    for (const eng::NetEvent& event : events) {
        switch (event.type) {
            case eng::NetEvent::Type::Connected: {
                eng::log::info("Connected; sending hello as '{}'", player_name_);
                eng::ByteWriter writer;
                write(writer, ClientHello{player_name_});
                net_.send(server_peer_, writer.data(), eng::NetChannel::Reliable, true);
                state_ = State::AwaitingWelcome;
                break;
            }
            case eng::NetEvent::Type::Disconnected:
                eng::log::warn("Disconnected from server");
                state_ = (state_ == State::Rejected) ? State::Rejected : State::Disconnected;
                break;
            case eng::NetEvent::Type::Message:
                handle_message(event.data);
                break;
        }
    }
}

void NetClient::handle_message(const std::vector<std::uint8_t>& data) {
    eng::ByteReader reader{{data.data(), data.size()}};
    const auto type = read_message_type(reader);
    if (!type) {
        eng::log::warn("Server sent an unknown message type; ignoring");
        return;
    }
    switch (*type) {
        case MessageType::ServerWelcome: {
            if (const auto welcome = read_server_welcome(reader)) {
                my_id_ = welcome->player_id;
                latest_server_tick_ = welcome->server_tick;
                state_ = State::InGame;
                players_[my_id_].name = player_name_;
                eng::log::info("Welcome: player {} on '{}' (tick {}, {} Hz / {} Hz snapshots)",
                               welcome->player_id, welcome->map, welcome->server_tick,
                               welcome->tick_rate, welcome->snapshot_rate);
            }
            break;
        }
        case MessageType::ServerReject: {
            if (const auto reject = read_server_reject(reader)) {
                eng::log::error("Server rejected connection (reason {})",
                                static_cast<int>(reject->reason));
                state_ = State::Rejected;
            }
            break;
        }
        case MessageType::PlayerJoined: {
            if (const auto joined = read_player_joined(reader)) {
                players_[joined->player_id].name = joined->name;
                eng::log::info("Player {} '{}' joined", joined->player_id, joined->name);
            }
            break;
        }
        case MessageType::PlayerLeft: {
            if (const auto left = read_player_left(reader)) {
                if (const auto it = players_.find(left->player_id); it != players_.end()) {
                    eng::log::info("Player {} '{}' left", left->player_id, it->second.name);
                    players_.erase(it);
                }
            }
            break;
        }
        case MessageType::Snapshot: {
            const auto snapshot = read_snapshot(reader);
            if (!snapshot || snapshot->server_tick <= latest_server_tick_) {
                return;  // stale, duplicate, or malformed
            }
            latest_server_tick_ = snapshot->server_tick;
            last_processed_input_ = snapshot->last_processed_input;
            for (auto& [id, player] : players_) {
                player.seen_in_snapshot = false;
            }
            for (const SnapshotPlayer& sp : snapshot->players) {
                NetPlayer& player = players_[sp.player_id];  // creates if unseen
                player.position = sp.position;
                player.velocity = sp.velocity;
                player.yaw = sp.yaw;
                player.pitch = sp.pitch;
                player.on_ground = (sp.flags & 1u) != 0;
                player.seen_in_snapshot = true;
                if (sp.player_id == my_id_) {
                    pending_self_ack_ =
                        SelfAck{sp.position, sp.velocity, player.on_ground,
                                snapshot->last_processed_input, snapshot->server_tick};
                } else {
                    player.history.push(RemoteSample{snapshot->server_tick, sp.position, sp.yaw,
                                                     sp.pitch, sp.flags});
                }
            }
            break;
        }
        case MessageType::FireEvent: {
            if (const auto m = read_fire_event(reader)) {
                fire_events_.push_back(*m);
            }
            break;
        }
        case MessageType::PlayerDamaged: {
            if (const auto m = read_player_damaged(reader)) {
                if (m->victim == my_id_) {
                    self_health_ = m->health;
                }
            }
            break;
        }
        case MessageType::PlayerDied: {
            if (const auto m = read_player_died(reader)) {
                if (m->victim == my_id_) {
                    self_alive_ = false;
                    self_health_ = 0.0f;
                }
                death_events_.push_back(*m);
            }
            break;
        }
        case MessageType::PlayerRespawned: {
            if (const auto m = read_player_respawned(reader)) {
                if (m->player == my_id_) {
                    self_alive_ = true;
                    self_health_ = 100.0f;
                }
                respawn_events_.push_back(*m);
            }
            break;
        }
        case MessageType::ScoreUpdate: {
            if (const auto m = read_score_update(reader)) {
                scores_[m->player] = {m->kills, m->deaths};
            }
            break;
        }
        case MessageType::MatchState: {
            if (const auto m = read_match_state(reader)) {
                match_phase_ = m->phase;
                match_seconds_ = m->seconds_remaining;
            }
            break;
        }
        case MessageType::WeaponStatus: {
            if (const auto m = read_weapon_status(reader)) {
                self_ammo_ = m->ammo;
                self_reloading_ = m->reloading;
            }
            break;
        }
        case MessageType::ClientHello:
        case MessageType::Input:
            break;  // client-to-server only; a server sending these is broken
    }
}

std::vector<FireEventMsg> NetClient::take_fire_events() {
    return std::exchange(fire_events_, {});
}

std::vector<PlayerDiedMsg> NetClient::take_death_events() {
    return std::exchange(death_events_, {});
}

std::vector<PlayerRespawnedMsg> NetClient::take_respawn_events() {
    return std::exchange(respawn_events_, {});
}

std::optional<SelfAck> NetClient::take_self_ack() {
    auto ack = pending_self_ack_;
    pending_self_ack_.reset();
    return ack;
}

void NetClient::send_input(const std::deque<InputCommand>& recent_newest_first,
                           std::uint32_t client_tick, std::uint32_t view_tick) {
    if (state_ != State::InGame || recent_newest_first.empty()) {
        return;
    }
    InputPacket packet;
    packet.newest_sequence = recent_newest_first.front().sequence;
    packet.client_tick = client_tick;
    packet.view_tick = view_tick;
    const std::size_t count = std::min(recent_newest_first.size(), kInputRedundancy);
    // Wire order is oldest -> newest.
    for (std::size_t i = count; i-- > 0;) {
        packet.commands.push_back(recent_newest_first[i]);
    }
    eng::ByteWriter writer;
    write(writer, packet);
    net_.send(server_peer_, writer.data(), eng::NetChannel::Sequenced, false);
}

}  // namespace game
