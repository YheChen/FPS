#include "game/server/server_game.h"

#include <cmath>
#include <utility>

#include "engine/core/log.h"
#include "game/shared/hitscan.h"

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
                       std::vector<glm::vec3> spawns, std::string map_name,
                       WeaponConfig weapon_config)
    : spawns_(std::move(spawns)),
      map_name_(std::move(map_name)),
      weapon_config_(std::move(weapon_config)) {
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
    player.weapon.ammo = weapon_config_.magazine_size;
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
            net.send(peer, encode(PlayerJoined{i, players_[i]->name}), eng::NetChannel::Reliable,
                     true);
        }
    }
    const auto joined = encode(PlayerJoined{*slot, hello->name});
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (players_[i] && i != *slot) {
            net.send(players_[i]->peer, joined, eng::NetChannel::Reliable, true);
        }
    }

    // Bring the newcomer up to date: match state, all scores, their weapon.
    net.send(peer, encode(match_state()), eng::NetChannel::Reliable, true);
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        if (players_[i]) {
            net.send(peer, encode(ScoreUpdateMsg{i, players_[i]->kills, players_[i]->deaths}),
                     eng::NetChannel::Reliable, true);
        }
    }
    send_weapon_status(*players_[*slot], net);
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

    // Newest claimed view tick for lag compensation (monotonic; clamped
    // again into the legal rewind window at fire time).
    if (packet->view_tick > player.view_tick) {
        player.view_tick = packet->view_tick;
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
    update_match(net);

    for (std::uint8_t id = 0; id < kMaxPlayers; ++id) {
        if (!players_[id]) {
            continue;
        }
        Player& player = *players_[id];

        // Consume exactly one input per tick, in order. When starved (loss
        // or jitter), reuse the last command briefly, then fast-forward.
        // Inputs are consumed even while dead so sequence acks keep flowing.
        const auto next = player.pending.find(player.last_processed_input + 1);
        InputCommand command = player.last_command;
        if (next != player.pending.end()) {
            command = next->second;
            player.pending.erase(next);
            player.last_processed_input = command.sequence;
            player.starved_ticks = 0;
        } else if (!player.pending.empty() && ++player.starved_ticks >= kStarvationJumpTicks) {
            const auto first = player.pending.begin();
            command = first->second;
            player.last_processed_input = first->first;
            player.pending.erase(first);
            player.starved_ticks = 0;
        }
        player.last_command = command;
        player.view_yaw = command.yaw;
        player.view_pitch = command.pitch;

        if (!player.alive) {
            player.respawn_remaining -= kTickSeconds;
            if (player.respawn_remaining <= 0.0f) {
                respawn_player(id, net);
            }
            continue;
        }

        advance_player(player.state, command, kTickSeconds, *player.controller, world_);
        player.history.push(tick_, player.state.position);

        // Fell out of the world: counts as an environment death.
        if (player.state.position.y < -20.0f) {
            kill_player(id, kNoPlayer, net);
            continue;
        }

        // Weapon: entirely authoritative. Fire rate, ammo, and reload come
        // from the deterministic shared state machine; a cheating client
        // holding Fire every tick still fires at exactly the configured RPM.
        if (phase_ == MatchPhase::Playing) {
            const bool fire_held = has_button(command, Button::Fire);
            const bool reload = has_button(command, Button::Reload);
            const WeaponTickResult shot =
                update_weapon(player.weapon, weapon_config_, fire_held, reload, kTickSeconds);
            if (shot.fired) {
                fire_hitscan(id, command, net);
            }
            if (shot.fired || shot.reload_started || shot.reload_finished) {
                send_weapon_status(player, net);
            }
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
        sp.flags = (p.state.on_ground ? kFlagOnGround : 0u) | (p.alive ? kFlagAlive : 0u);
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

// --- combat -----------------------------------------------------------------

void ServerGame::broadcast_reliable(const std::vector<std::uint8_t>& data, eng::NetHost& net) {
    for (const auto& player : players_) {
        if (player) {
            net.send(player->peer, data, eng::NetChannel::Reliable, true);
        }
    }
}

void ServerGame::send_weapon_status(const Player& player, eng::NetHost& net) {
    net.send(player.peer,
             encode(WeaponStatusMsg{static_cast<std::uint8_t>(player.weapon.ammo),
                                    player.weapon.reloading()}),
             eng::NetChannel::Reliable, true);
}

MatchStateMsg ServerGame::match_state() const {
    MatchStateMsg m;
    m.phase = phase_;
    m.seconds_remaining = static_cast<std::uint16_t>(
        std::max(0.0f, phase_ == MatchPhase::Playing ? match_remaining_ : restart_remaining_));
    return m;
}

void ServerGame::fire_hitscan(std::uint8_t shooter_id, const InputCommand& command,
                              eng::NetHost& net) {
    Player& shooter = *players_[shooter_id];
    const glm::vec3 eye = shooter.state.position + glm::vec3{0.0f, kMove.eye_height, 0.0f};
    const glm::vec3 dir = view_direction(command.yaw, command.pitch);

    // World geometry bounds the shot.
    float max_t = weapon_config_.range;
    if (const auto wall = world_.raycast(eye, dir, weapon_config_.range)) {
        max_t = wall->distance;
    }

    // Lag compensation: test victims where the SHOOTER saw them - their
    // positions at the shooter's (bounded) interpolation view tick. A
    // hostile view_tick claim can rewind at most kMaxRewindTicks (~250 ms).
    const std::uint32_t rewind_tick = clamp_rewind_tick(shooter.view_tick, tick_);
    std::uint8_t hit_id = kNoPlayer;
    float hit_t = max_t;
    for (std::uint8_t id = 0; id < kMaxPlayers; ++id) {
        if (id == shooter_id || !players_[id] || !players_[id]->alive) {
            continue;
        }
        const glm::vec3 victim_position =
            players_[id]->history.at(rewind_tick).value_or(players_[id]->state.position);
        const auto& config = players_[id]->controller->config();
        const auto t =
            ray_vertical_capsule(eye, dir, victim_position, config.radius, config.height);
        if (t && *t < hit_t) {
            hit_t = *t;
            hit_id = id;
        }
    }
    if (rewind_tick != tick_ && hit_id != kNoPlayer) {
        eng::log::debug("lag comp: hit resolved {} ticks in the past", tick_ - rewind_tick);
    }

    FireEventMsg event;
    event.shooter = shooter_id;
    event.from = eye;
    event.to = eye + dir * hit_t;
    event.hit_player = hit_id;
    broadcast_reliable(encode(event), net);

    if (hit_id != kNoPlayer) {
        Player& victim = *players_[hit_id];
        const bool died = apply_damage(victim.health, weapon_config_.damage);
        broadcast_reliable(encode(PlayerDamagedMsg{hit_id, shooter_id, victim.health.current}),
                           net);
        if (died) {
            kill_player(hit_id, shooter_id, net);
        }
    }
}

void ServerGame::kill_player(std::uint8_t victim_id, std::uint8_t killer_id, eng::NetHost& net) {
    Player& victim = *players_[victim_id];
    victim.alive = false;
    victim.health.current = 0.0f;
    victim.respawn_remaining = kRespawnSeconds;
    victim.state.velocity = {0.0f, 0.0f, 0.0f};
    ++victim.deaths;
    if (killer_id != kNoPlayer && players_[killer_id]) {
        ++players_[killer_id]->kills;
        broadcast_reliable(encode(ScoreUpdateMsg{killer_id, players_[killer_id]->kills,
                                                 players_[killer_id]->deaths}),
                           net);
    }
    broadcast_reliable(encode(PlayerDiedMsg{victim_id, killer_id}), net);
    broadcast_reliable(encode(ScoreUpdateMsg{victim_id, victim.kills, victim.deaths}), net);
    eng::log::info("Player {} '{}' killed by {}", victim_id, victim.name,
                   killer_id == kNoPlayer ? -1 : static_cast<int>(killer_id));
}

void ServerGame::respawn_player(std::uint8_t player_id, eng::NetHost& net) {
    Player& player = *players_[player_id];
    const glm::vec3 spawn = spawns_[next_spawn_++ % spawns_.size()];
    player.state = {};
    player.state.position = spawn;
    player.controller->set_position(spawn);
    player.controller->set_velocity({0.0f, 0.0f, 0.0f});
    reset_health(player.health);
    player.weapon = {};
    player.weapon.ammo = weapon_config_.magazine_size;
    player.alive = true;
    broadcast_reliable(encode(PlayerRespawnedMsg{player_id, spawn}), net);
    send_weapon_status(player, net);
}

void ServerGame::update_match(eng::NetHost& net) {
    if (phase_ == MatchPhase::Playing) {
        match_remaining_ -= kTickSeconds;
        if (match_remaining_ <= 0.0f) {
            phase_ = MatchPhase::Ended;
            restart_remaining_ = kMatchRestartSeconds;
            broadcast_reliable(encode(match_state()), net);
            eng::log::info("Match ended");
        }
    } else {
        restart_remaining_ -= kTickSeconds;
        if (restart_remaining_ <= 0.0f) {
            restart_match(net);
        }
    }
    // Periodic timer sync (1 Hz).
    if (tick_ % 60 == 0) {
        broadcast_reliable(encode(match_state()), net);
    }
}

void ServerGame::restart_match(eng::NetHost& net) {
    phase_ = MatchPhase::Playing;
    match_remaining_ = kMatchSeconds;
    for (std::uint8_t id = 0; id < kMaxPlayers; ++id) {
        if (!players_[id]) {
            continue;
        }
        players_[id]->kills = 0;
        players_[id]->deaths = 0;
        broadcast_reliable(encode(ScoreUpdateMsg{id, 0, 0}), net);
        respawn_player(id, net);
    }
    broadcast_reliable(encode(match_state()), net);
    eng::log::info("Match restarted");
}

}  // namespace game
