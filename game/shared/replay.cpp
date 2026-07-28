#include "game/shared/replay.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include "engine/core/log.h"
#include "engine/net/byte_buffer.h"

namespace game {

namespace {

// "FPSR". Written as four bytes so the file is recognisable in a hex dump and
// so endianness plays no part in identifying it.
constexpr std::uint8_t kMagic[4] = {'F', 'P', 'S', 'R'};

constexpr std::size_t kMaxMapPathLength = 128;

void write_command(eng::ByteWriter& writer, const ReplayCommand& entry) {
    writer.u8(entry.player_id);
    writer.u32(entry.command.sequence);
    writer.f32(entry.command.yaw);
    writer.f32(entry.command.pitch);
    writer.u16(entry.command.buttons);
    writer.u8(entry.command.weapon_slot);
}

std::optional<ReplayCommand> read_command(eng::ByteReader& reader) {
    const auto player_id = reader.u8();
    const auto sequence = reader.u32();
    const auto yaw = reader.f32();
    const auto pitch = reader.f32();
    const auto buttons = reader.u16();
    const auto weapon_slot = reader.u8();
    // A poisoned reader means the file ran out mid-command; checking ok()
    // once is enough, because the first failed read poisons every read after
    // it (see byte_buffer.h).
    if (!reader.ok()) {
        return std::nullopt;
    }

    ReplayCommand entry;
    entry.player_id = *player_id;
    entry.command.sequence = *sequence;
    entry.command.yaw = *yaw;
    entry.command.pitch = *pitch;
    entry.command.buttons = *buttons;
    entry.command.weapon_slot = *weapon_slot;

    // Same validation the wire protocol applies: a replay file is just as
    // untrusted as a packet.
    if (entry.player_id >= kMaxPlayers || entry.command.weapon_slot >= kMaxWeapons) {
        return std::nullopt;
    }
    return entry;
}

}  // namespace

const ReplayPlayer* Replay::find_player(std::uint8_t id) const {
    const auto found = std::find_if(players.begin(), players.end(),
                                    [id](const ReplayPlayer& p) { return p.id == id; });
    return found == players.end() ? nullptr : &*found;
}

std::vector<std::uint8_t> encode_replay(const Replay& replay) {
    eng::ByteWriter writer;
    for (const std::uint8_t byte : kMagic) {
        writer.u8(byte);
    }
    writer.u16(kReplayVersion);
    writer.u16(replay.tick_rate_hz);
    writer.str(replay.map_path);

    writer.u8(static_cast<std::uint8_t>(std::min<std::size_t>(replay.players.size(), kMaxPlayers)));
    for (std::size_t i = 0; i < replay.players.size() && i < kMaxPlayers; ++i) {
        const ReplayPlayer& player = replay.players[i];
        writer.u8(player.id);
        writer.str(player.name);
        writer.f32(player.spawn.x);
        writer.f32(player.spawn.y);
        writer.f32(player.spawn.z);
    }

    writer.u32(static_cast<std::uint32_t>(replay.frames.size()));
    for (const ReplayFrame& frame : replay.frames) {
        writer.u32(frame.tick);
        writer.u8(
            static_cast<std::uint8_t>(std::min<std::size_t>(frame.commands.size(), kMaxPlayers)));
        for (std::size_t i = 0; i < frame.commands.size() && i < kMaxPlayers; ++i) {
            write_command(writer, frame.commands[i]);
        }
    }

    const std::span<const std::uint8_t> data = writer.data();
    return {data.begin(), data.end()};
}

std::optional<Replay> decode_replay(std::span<const std::uint8_t> bytes) {
    eng::ByteReader reader{bytes};

    for (const std::uint8_t expected : kMagic) {
        const auto byte = reader.u8();
        if (!byte || *byte != expected) {
            eng::log::error("Replay: bad magic; not a replay file");
            return std::nullopt;
        }
    }

    const auto version = reader.u16();
    if (!version) {
        return std::nullopt;
    }
    if (*version != kReplayVersion) {
        // Guessing at an older layout is how you get a replay that plays back
        // subtly wrong, which is worse than refusing it.
        eng::log::error("Replay: version {} is not supported (expected {})", *version,
                        kReplayVersion);
        return std::nullopt;
    }

    Replay replay;
    const auto tick_rate = reader.u16();
    auto map_path = reader.str(kMaxMapPathLength);
    if (!tick_rate || *tick_rate == 0 || !map_path) {
        eng::log::error("Replay: invalid header");
        return std::nullopt;
    }
    replay.tick_rate_hz = *tick_rate;
    replay.map_path = std::move(*map_path);

    const auto player_count = reader.u8();
    if (!player_count || *player_count > kMaxPlayers) {
        eng::log::error("Replay: invalid player count");
        return std::nullopt;
    }
    replay.players.reserve(*player_count);
    for (std::uint8_t i = 0; i < *player_count; ++i) {
        ReplayPlayer player;
        const auto id = reader.u8();
        auto name = reader.str(kMaxNameLength);
        const auto x = reader.f32();
        const auto y = reader.f32();
        const auto z = reader.f32();
        if (!id || *id >= kMaxPlayers || !name || !x || !y || !z) {
            eng::log::error("Replay: invalid player entry {}", i);
            return std::nullopt;
        }
        player.id = *id;
        player.name = std::move(*name);
        player.spawn = {*x, *y, *z};
        replay.players.push_back(std::move(player));
    }

    const auto frame_count = reader.u32();
    if (!frame_count || *frame_count > kMaxReplayFrames) {
        eng::log::error("Replay: invalid frame count");
        return std::nullopt;
    }
    replay.frames.reserve(*frame_count);

    bool first_frame = true;
    std::uint32_t previous_tick = 0;
    for (std::uint32_t f = 0; f < *frame_count; ++f) {
        ReplayFrame frame;
        const auto tick = reader.u32();
        const auto command_count = reader.u8();
        if (!tick || !command_count || *command_count > kMaxPlayers) {
            eng::log::error("Replay: invalid frame {}", f);
            return std::nullopt;
        }
        // Ticks must advance. A file whose ticks jump backwards cannot be
        // replayed in order, so it is rejected rather than sorted silently.
        if (!first_frame && *tick <= previous_tick) {
            eng::log::error("Replay: frame {} tick {} does not follow {}", f, *tick, previous_tick);
            return std::nullopt;
        }
        first_frame = false;
        previous_tick = *tick;

        frame.tick = *tick;
        frame.commands.reserve(*command_count);
        for (std::uint8_t c = 0; c < *command_count; ++c) {
            auto entry = read_command(reader);
            if (!entry) {
                eng::log::error("Replay: invalid command in frame {}", f);
                return std::nullopt;
            }
            frame.commands.push_back(*entry);
        }
        replay.frames.push_back(std::move(frame));
    }

    if (!reader.finished()) {
        eng::log::error("Replay: {} trailing bytes", reader.remaining());
        return std::nullopt;
    }
    return replay;
}

bool write_replay_file(const std::filesystem::path& path, const Replay& replay) {
    const std::vector<std::uint8_t> bytes = encode_replay(replay);

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        eng::log::error("Replay: cannot open '{}' for writing", path.string());
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        eng::log::error("Replay: write to '{}' failed", path.string());
        return false;
    }
    eng::log::info("Replay: wrote {} ({} frames, {} bytes)", path.string(), replay.frames.size(),
                   bytes.size());
    return true;
}

std::optional<Replay> read_replay_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        eng::log::error("Replay: cannot open '{}'", path.string());
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                                          std::istreambuf_iterator<char>()};
    return decode_replay(bytes);
}

void ReplayRecorder::begin(std::string map_path, std::uint16_t tick_rate_hz) {
    replay_ = Replay{};
    replay_.map_path = std::move(map_path);
    replay_.tick_rate_hz = tick_rate_hz;
    recording_ = true;
}

void ReplayRecorder::add_player(std::uint8_t id, std::string name, const glm::vec3& spawn) {
    if (!recording_) {
        return;
    }
    const auto found = std::find_if(replay_.players.begin(), replay_.players.end(),
                                    [id](const ReplayPlayer& p) { return p.id == id; });
    if (found != replay_.players.end()) {
        found->name = std::move(name);
        found->spawn = spawn;
        return;
    }
    if (replay_.players.size() >= kMaxPlayers) {
        return;
    }
    replay_.players.push_back({id, std::move(name), spawn});
}

void ReplayRecorder::record(std::uint32_t tick, std::uint8_t player_id,
                            const InputCommand& command) {
    if (!recording_ || player_id >= kMaxPlayers) {
        return;
    }
    if (!replay_.frames.empty() && tick < replay_.frames.back().tick) {
        return;  // out of order; replaying it would not match what ran
    }
    if (replay_.frames.empty() || replay_.frames.back().tick != tick) {
        if (replay_.frames.size() >= kMaxReplayFrames) {
            return;
        }
        replay_.frames.push_back(ReplayFrame{tick, {}});
    }
    ReplayFrame& frame = replay_.frames.back();
    if (frame.commands.size() >= kMaxPlayers) {
        return;
    }
    frame.commands.push_back({player_id, command});
}

}  // namespace game
