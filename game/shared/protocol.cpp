#include "game/shared/protocol.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace game {

namespace {

constexpr float kMaxPitch = 1.5533f;  // ~89 degrees
constexpr float kPi = std::numbers::pi_v<float>;

void write_command_body(eng::ByteWriter& w, const InputCommand& c) {
    w.f32(c.yaw);
    w.f32(c.pitch);
    w.u8(c.buttons);
}

// Reads one command body; validates and normalizes angles.
std::optional<InputCommand> read_command_body(eng::ByteReader& r) {
    InputCommand c;
    const auto yaw = r.f32();
    const auto pitch = r.f32();
    const auto buttons = r.u8();
    if (!yaw || !pitch || !buttons) {
        return std::nullopt;
    }
    if (*yaw < -8.0f || *yaw > 8.0f || *pitch < -kMaxPitch - 0.01f || *pitch > kMaxPitch + 0.01f) {
        return std::nullopt;  // outside any legitimate range
    }
    c.yaw = std::remainder(*yaw, 2.0f * kPi);
    c.pitch = std::clamp(*pitch, -kMaxPitch, kMaxPitch);
    c.buttons = *buttons;
    return c;
}

}  // namespace

// --- encode -----------------------------------------------------------------

void write(eng::ByteWriter& w, const ClientHello& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::ClientHello));
    w.u16(kProtocolVersion);
    w.str(m.name);
}

void write(eng::ByteWriter& w, const ServerWelcome& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::ServerWelcome));
    w.u8(m.player_id);
    w.u8(m.tick_rate);
    w.u8(m.snapshot_rate);
    w.u32(m.server_tick);
    w.str(m.map);
}

void write(eng::ByteWriter& w, const ServerReject& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::ServerReject));
    w.u8(static_cast<std::uint8_t>(m.reason));
}

void write(eng::ByteWriter& w, const PlayerJoined& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::PlayerJoined));
    w.u8(m.player_id);
    w.str(m.name);
}

void write(eng::ByteWriter& w, const PlayerLeft& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::PlayerLeft));
    w.u8(m.player_id);
}

void write(eng::ByteWriter& w, const InputPacket& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::Input));
    w.u32(m.newest_sequence);
    w.u32(m.client_tick);
    w.u32(m.view_tick);
    w.u8(static_cast<std::uint8_t>(m.commands.size()));
    for (const InputCommand& c : m.commands) {
        write_command_body(w, c);
    }
}

void write(eng::ByteWriter& w, const Snapshot& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::Snapshot));
    w.u32(m.server_tick);
    w.u32(m.last_processed_input);
    w.u8(static_cast<std::uint8_t>(m.players.size()));
    for (const SnapshotPlayer& p : m.players) {
        w.u8(p.player_id);
        w.f32(p.position.x);
        w.f32(p.position.y);
        w.f32(p.position.z);
        w.f32(p.velocity.x);
        w.f32(p.velocity.y);
        w.f32(p.velocity.z);
        w.f32(p.yaw);
        w.f32(p.pitch);
        w.u8(p.flags);
    }
}

void write(eng::ByteWriter& w, const FireEventMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::FireEvent));
    w.u8(m.shooter);
    w.f32(m.from.x);
    w.f32(m.from.y);
    w.f32(m.from.z);
    w.f32(m.to.x);
    w.f32(m.to.y);
    w.f32(m.to.z);
    w.u8(m.hit_player);
}

void write(eng::ByteWriter& w, const PlayerDamagedMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::PlayerDamaged));
    w.u8(m.victim);
    w.u8(m.attacker);
    w.f32(m.health);
}

void write(eng::ByteWriter& w, const PlayerDiedMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::PlayerDied));
    w.u8(m.victim);
    w.u8(m.killer);
}

void write(eng::ByteWriter& w, const PlayerRespawnedMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::PlayerRespawned));
    w.u8(m.player);
    w.f32(m.position.x);
    w.f32(m.position.y);
    w.f32(m.position.z);
}

void write(eng::ByteWriter& w, const ScoreUpdateMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::ScoreUpdate));
    w.u8(m.player);
    w.u16(m.kills);
    w.u16(m.deaths);
}

void write(eng::ByteWriter& w, const MatchStateMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::MatchState));
    w.u8(static_cast<std::uint8_t>(m.phase));
    w.u16(m.seconds_remaining);
}

void write(eng::ByteWriter& w, const WeaponStatusMsg& m) {
    w.u8(static_cast<std::uint8_t>(MessageType::WeaponStatus));
    w.u8(m.ammo);
    w.u8(m.reloading ? 1u : 0u);
}

// --- decode -----------------------------------------------------------------

std::optional<MessageType> read_message_type(eng::ByteReader& r) {
    const auto value = r.u8();
    if (!value || *value < static_cast<std::uint8_t>(MessageType::ClientHello) ||
        *value > static_cast<std::uint8_t>(MessageType::WeaponStatus)) {
        return std::nullopt;
    }
    return static_cast<MessageType>(*value);
}

std::optional<ClientHello> read_client_hello(eng::ByteReader& r) {
    const auto version = r.u16();
    if (!version || *version != kProtocolVersion) {
        return std::nullopt;
    }
    const auto name = r.str(kMaxNameLength);
    if (!name || !r.finished()) {
        return std::nullopt;
    }
    return ClientHello{*name};
}

std::optional<ServerWelcome> read_server_welcome(eng::ByteReader& r) {
    ServerWelcome m;
    const auto id = r.u8();
    const auto tick_rate = r.u8();
    const auto snapshot_rate = r.u8();
    const auto tick = r.u32();
    const auto map = r.str(64);
    if (!id || *id >= kMaxPlayers || !tick_rate || *tick_rate == 0 || !snapshot_rate ||
        *snapshot_rate == 0 || !tick || !map || !r.finished()) {
        return std::nullopt;
    }
    m.player_id = *id;
    m.tick_rate = *tick_rate;
    m.snapshot_rate = *snapshot_rate;
    m.server_tick = *tick;
    m.map = *map;
    return m;
}

std::optional<ServerReject> read_server_reject(eng::ByteReader& r) {
    const auto reason = r.u8();
    if (!reason || *reason < 1 || *reason > 3 || !r.finished()) {
        return std::nullopt;
    }
    return ServerReject{static_cast<RejectReason>(*reason)};
}

std::optional<PlayerJoined> read_player_joined(eng::ByteReader& r) {
    const auto id = r.u8();
    const auto name = r.str(kMaxNameLength);
    if (!id || *id >= kMaxPlayers || !name || !r.finished()) {
        return std::nullopt;
    }
    return PlayerJoined{*id, *name};
}

std::optional<PlayerLeft> read_player_left(eng::ByteReader& r) {
    const auto id = r.u8();
    if (!id || *id >= kMaxPlayers || !r.finished()) {
        return std::nullopt;
    }
    return PlayerLeft{*id};
}

std::optional<InputPacket> read_input_packet(eng::ByteReader& r) {
    InputPacket m;
    const auto newest = r.u32();
    const auto client_tick = r.u32();
    const auto view_tick = r.u32();
    const auto count = r.u8();
    if (!newest || !client_tick || !view_tick || !count || *count == 0 ||
        *count > kInputRedundancy) {
        return std::nullopt;
    }
    if (*newest + 1 < static_cast<std::uint32_t>(*count)) {
        return std::nullopt;  // sequences would underflow
    }
    m.newest_sequence = *newest;
    m.client_tick = *client_tick;
    m.view_tick = *view_tick;
    for (std::uint8_t i = 0; i < *count; ++i) {
        auto command = read_command_body(r);
        if (!command) {
            return std::nullopt;
        }
        // commands[0] is oldest: newest - (count-1) ... newest.
        command->sequence = *newest - (*count - 1u - i);
        m.commands.push_back(*command);
    }
    if (!r.finished()) {
        return std::nullopt;
    }
    return m;
}

std::optional<Snapshot> read_snapshot(eng::ByteReader& r) {
    Snapshot m;
    const auto tick = r.u32();
    const auto last_input = r.u32();
    const auto count = r.u8();
    if (!tick || !last_input || !count || *count > kMaxPlayers) {
        return std::nullopt;
    }
    m.server_tick = *tick;
    m.last_processed_input = *last_input;
    for (std::uint8_t i = 0; i < *count; ++i) {
        SnapshotPlayer p;
        const auto id = r.u8();
        const auto px = r.f32();
        const auto py = r.f32();
        const auto pz = r.f32();
        const auto vx = r.f32();
        const auto vy = r.f32();
        const auto vz = r.f32();
        const auto yaw = r.f32();
        const auto pitch = r.f32();
        const auto flags = r.u8();
        if (!id || *id >= kMaxPlayers || !px || !py || !pz || !vx || !vy || !vz || !yaw || !pitch ||
            !flags.has_value()) {
            return std::nullopt;
        }
        p.player_id = *id;
        p.position = {*px, *py, *pz};
        p.velocity = {*vx, *vy, *vz};
        p.yaw = *yaw;
        p.pitch = *pitch;
        p.flags = *flags;
        m.players.push_back(p);
    }
    if (!r.finished()) {
        return std::nullopt;
    }
    return m;
}

namespace {

bool valid_player_or_none(std::uint8_t id) {
    return id < kMaxPlayers || id == kNoPlayer;
}

}  // namespace

std::optional<FireEventMsg> read_fire_event(eng::ByteReader& r) {
    FireEventMsg m;
    const auto shooter = r.u8();
    const auto fx = r.f32();
    const auto fy = r.f32();
    const auto fz = r.f32();
    const auto tx = r.f32();
    const auto ty = r.f32();
    const auto tz = r.f32();
    const auto hit = r.u8();
    if (!shooter || *shooter >= kMaxPlayers || !fx || !fy || !fz || !tx || !ty || !tz || !hit ||
        !valid_player_or_none(*hit) || !r.finished()) {
        return std::nullopt;
    }
    m.shooter = *shooter;
    m.from = {*fx, *fy, *fz};
    m.to = {*tx, *ty, *tz};
    m.hit_player = *hit;
    return m;
}

std::optional<PlayerDamagedMsg> read_player_damaged(eng::ByteReader& r) {
    const auto victim = r.u8();
    const auto attacker = r.u8();
    const auto health = r.f32();
    if (!victim || *victim >= kMaxPlayers || !attacker || !valid_player_or_none(*attacker) ||
        !health || *health < 0.0f || *health > 1000.0f || !r.finished()) {
        return std::nullopt;
    }
    return PlayerDamagedMsg{*victim, *attacker, *health};
}

std::optional<PlayerDiedMsg> read_player_died(eng::ByteReader& r) {
    const auto victim = r.u8();
    const auto killer = r.u8();
    if (!victim || *victim >= kMaxPlayers || !killer || !valid_player_or_none(*killer) ||
        !r.finished()) {
        return std::nullopt;
    }
    return PlayerDiedMsg{*victim, *killer};
}

std::optional<PlayerRespawnedMsg> read_player_respawned(eng::ByteReader& r) {
    const auto player = r.u8();
    const auto x = r.f32();
    const auto y = r.f32();
    const auto z = r.f32();
    if (!player || *player >= kMaxPlayers || !x || !y || !z || !r.finished()) {
        return std::nullopt;
    }
    return PlayerRespawnedMsg{*player, {*x, *y, *z}};
}

std::optional<ScoreUpdateMsg> read_score_update(eng::ByteReader& r) {
    const auto player = r.u8();
    const auto kills = r.u16();
    const auto deaths = r.u16();
    if (!player || *player >= kMaxPlayers || !kills || !deaths || !r.finished()) {
        return std::nullopt;
    }
    return ScoreUpdateMsg{*player, *kills, *deaths};
}

std::optional<MatchStateMsg> read_match_state(eng::ByteReader& r) {
    const auto phase = r.u8();
    const auto seconds = r.u16();
    if (!phase || *phase < 1 || *phase > 2 || !seconds || !r.finished()) {
        return std::nullopt;
    }
    return MatchStateMsg{static_cast<MatchPhase>(*phase), *seconds};
}

std::optional<WeaponStatusMsg> read_weapon_status(eng::ByteReader& r) {
    const auto ammo = r.u8();
    const auto reloading = r.u8();
    if (!ammo.has_value() || !reloading || *reloading > 1 || !r.finished()) {
        return std::nullopt;
    }
    return WeaponStatusMsg{*ammo, *reloading == 1};
}

}  // namespace game
