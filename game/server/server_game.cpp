#include "game/server/server_game.h"

#include <utility>

#include "engine/core/log.h"

namespace game {

namespace {

constexpr int kMaxInputPacketsPerSecond = 200;
constexpr int kMaxBadMessages = 10;
constexpr int kStarvationJumpTicks = 6;

template <typename Message>
std::vector<std::uint8_t> encode(const Message& message) {
    eng::ByteWriter writer;
    write(writer, message);
    return {writer.data().begin(), writer.data().end()};
}

}  // namespace

ServerGame::ServerGame(std::vector<std::pair<eng::MeshData, glm::mat4>> map_meshes,
                       std::vector<glm::vec3> spawns, std::string map_name)
    : spawns_(std::move(spawns)), map_name_(std::move(map_name)) {
    for (const auto& [mesh, transform] : map_meshes) {
        world_.add_static_mesh(mesh, transform);
    }
    world_.optimize();
    if (spawns_.empty()) {
        spawns_.push_back({0.0f, 1.0f, 0.0f});
    }
    eng::log::info("ServerGame: {} static bodies, {} spawns, map '{}'", world_.body_count(),
                   spawns_.size(), map_name_);
}

std::size_t ServerGame::player_count() const {
    std::size_t count = 0;
    for (const auto& player : players_) {
        if (player) {
            ++count;
        }
    }
    return count;
}

std::optional<std::uint8_t> ServerGame::find_player_by_peer(std::uint32_t peer) const {
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (players_[i] && players_[i]->peer == peer) {
            return i;
        }
    }
    return std::nullopt;
}

void ServerGame::handle_event(const eng::NetEvent& event, eng::NetHost& net) {
    switch (event.type) {
        case eng::NetEvent::Type::Connected:
            eng::log::info("Peer {} connected (awaiting hello)", event.peer);
            break;
        case eng::NetEvent::Type::Disconnected: {
            if (const auto id = find_player_by_peer(event.peer)) {
                eng::log::info("Player {} '{}' disconnected", *id, players_[*id]->name);
                drop_player(*id, net);
            }
            break;
        }
        case eng::NetEvent::Type::Message: {
            eng::ByteReader reader{{event.data.data(), event.data.size()}};
            const auto type = read_message_type(reader);
            const auto player_id = find_player_by_peer(event.peer);

            if (!type) {
                if (player_id) {
                    if (++players_[*player_id]->bad_messages > kMaxBadMessages) {
                        eng::log::warn("Player {}: too many bad messages, kicking", *player_id);
                        net.disconnect(event.peer);
                    }
                }
                return;
            }

            if (*type == MessageType::ClientHello) {
                handle_hello(event.peer, reader, net);
            } else if (*type == MessageType::Input && player_id) {
                handle_input(*players_[*player_id], reader, net);
            }
            // Anything else from a client is ignored (and counted).
            break;
        }
    }
}

void ServerGame::handle_hello(std::uint32_t peer, eng::ByteReader& reader, eng::NetHost& net) {
    const auto hello = read_client_hello(reader);
    if (!hello) {
        eng::log::warn("Peer {}: invalid hello (version/name), rejecting", peer);
        const auto reject = encode(ServerReject{RejectReason::VersionMismatch});
        net.send(peer, reject, eng::NetChannel::Reliable, true);
        net.disconnect(peer);
        return;
    }
    if (find_player_by_peer(peer)) {
        return;  // duplicate hello; ignore
    }

    std::optional<std::uint8_t> slot;
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (!players_[i]) {
            slot = i;
            break;
        }
    }
    if (!slot) {
        const auto reject = encode(ServerReject{RejectReason::ServerFull});
        net.send(peer, reject, eng::NetChannel::Reliable, true);
        net.disconnect(peer);
        return;
    }

    Player player;
    player.peer = peer;
    player.name = hello->name;
    const glm::vec3 spawn = spawns_[next_spawn_++ % spawns_.size()];
    player.state.position = spawn;
    player.controller = std::make_unique<eng::CharacterController>(world_, spawn);
    players_[*slot] = std::move(player);

    eng::log::info("Player {} '{}' joined (peer {})", *slot, hello->name, peer);

    // Welcome the new player.
    ServerWelcome welcome;
    welcome.player_id = *slot;
    welcome.server_tick = tick_;
    welcome.map = map_name_;
    net.send(peer, encode(welcome), eng::NetChannel::Reliable, true);

    // Tell them about everyone already here, and everyone about them.
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (players_[i] && i != *slot) {
            net.send(peer, encode(PlayerJoined{i, players_[i]->name}),
                     eng::NetChannel::Reliable, true);
        }
    }
    const auto joined = encode(PlayerJoined{*slot, hello->name});
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (players_[i] && i != *slot) {
            net.send(players_[i]->peer, joined, eng::NetChannel::Reliable, true);
        }
    }
}

void ServerGame::handle_input(Player& player, eng::ByteReader& reader, eng::NetHost& net) {
    if (++player.input_packets_this_second > kMaxInputPacketsPerSecond) {
        eng::log::warn("Player '{}': input rate limit exceeded, kicking", player.name);
        net.disconnect(player.peer);
        return;
    }

    const auto packet = read_input_packet(reader);
    if (!packet) {
        if (++player.bad_messages > kMaxBadMessages) {
            eng::log::warn("Player '{}': too many malformed inputs, kicking", player.name);
            net.disconnect(player.peer);
        }
        return;
    }

    // Reject inputs absurdly far ahead of what we've processed.
    if (packet->newest_sequence > player.last_processed_input + kMaxInputAhead) {
        return;
    }

    for (const InputCommand& command : packet->commands) {
        if (command.sequence > player.last_processed_input &&
            !player.pending.contains(command.sequence)) {
            player.pending.emplace(command.sequence, command);
        }
    }
}

void ServerGame::drop_player(std::uint8_t player_id, eng::NetHost& net) {
    players_[player_id].reset();
    const auto left = encode(PlayerLeft{player_id});
    for (const auto& player : players_) {
        if (player) {
            net.send(player->peer, left, eng::NetChannel::Reliable, true);
        }
    }
}

void ServerGame::tick(eng::NetHost& net) {
    ++tick_;

    for (auto& slot : players_) {
        if (!slot) {
            continue;
        }
        Player& player = *slot;

        // Consume exactly one input per tick, in order. When starved (loss
        // or jitter), reuse the last command briefly, then fast-forward.
        const auto next = player.pending.find(player.last_processed_input + 1);
        InputCommand command = player.last_command;
        if (next != player.pending.end()) {
            command = next->second;
            player.pending.erase(next);
            player.last_processed_input = command.sequence;
            player.starved_ticks = 0;
        } else if (!player.pending.empty() &&
                   ++player.starved_ticks >= kStarvationJumpTicks) {
            const auto first = player.pending.begin();
            command = first->second;
            player.last_processed_input = first->first;
            player.pending.erase(first);
            player.starved_ticks = 0;
        }
        player.last_command = command;
        player.view_yaw = command.yaw;
        player.view_pitch = command.pitch;

        advance_player(player.state, command, kTickSeconds, *player.controller, world_);

        // Fell out of the world: respawn.
        if (player.state.position.y < -20.0f) {
            player.state = {};
            player.state.position = spawns_[next_spawn_++ % spawns_.size()];
        }
    }

    // Reset per-second rate counters.
    if (tick_ % 60 == 0) {
        for (auto& slot : players_) {
            if (slot) {
                slot->input_packets_this_second = 0;
            }
        }
    }

    if (tick_ % kSnapshotDivisor == 0) {
        send_snapshots(net);
    }
}

void ServerGame::send_snapshots(eng::NetHost& net) {
    // Shared player list; per-recipient last_processed_input header.
    std::vector<SnapshotPlayer> everyone;
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (!players_[i]) {
            continue;
        }
        const Player& p = *players_[i];
        SnapshotPlayer sp;
        sp.player_id = i;
        sp.position = p.state.position;
        sp.velocity = p.state.velocity;
        sp.yaw = p.view_yaw;
        sp.pitch = p.view_pitch;
        sp.flags = p.state.on_ground ? 1u : 0u;
        everyone.push_back(sp);
    }

    for (const auto& slot : players_) {
        if (!slot) {
            continue;
        }
        Snapshot snapshot;
        snapshot.server_tick = tick_;
        snapshot.last_processed_input = slot->last_processed_input;
        snapshot.players = everyone;
        net.send(slot->peer, encode(snapshot), eng::NetChannel::Sequenced, false);
    }
}

}  // namespace game
