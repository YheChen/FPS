#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/shared/input_command.h"
#include "game/shared/protocol.h"
#include "game/shared/weapon.h"

// Match recording and playback.
//
// A replay stores **inputs only** -- never positions. The simulation is
// already deterministic (advance_player is a pure function of state + command,
// and game/shared/rng.h is a pure hash), so replaying the same inputs through
// the same code reproduces the match exactly. Recording positions would be
// larger, would drift out of sync with the code that produced them, and would
// quietly stop being a check on determinism at all.
//
// That property is the point: a replay that diverges is a determinism bug, and
// the test suite uses one as exactly that regression test.
//
// Cosmetic systems (particles, character animation) deliberately run on the
// render clock with their own RNG, so they are neither recorded nor replayed.
namespace game {

// Bumped whenever the layout below changes. Files carrying any other version
// are rejected rather than guessed at.
inline constexpr std::uint16_t kReplayVersion = 1;

// Guards against a corrupt header asking for a huge allocation.
inline constexpr std::uint32_t kMaxReplayFrames = 60u * 60u * 60u;  // one hour at 60 Hz

struct ReplayPlayer {
    std::uint8_t id = 0;
    std::string name;
    glm::vec3 spawn{0.0f};
};

struct ReplayCommand {
    std::uint8_t player_id = 0;
    InputCommand command;
};

// Every input the server applied on one tick. Ticks are stored explicitly
// rather than implied by position, so a frame where nobody sent input is
// still representable and gaps are detectable.
struct ReplayFrame {
    std::uint32_t tick = 0;
    std::vector<ReplayCommand> commands;
};

struct Replay {
    std::string map_path;
    std::uint16_t tick_rate_hz = 60;
    std::vector<ReplayPlayer> players;
    std::vector<ReplayFrame> frames;

    const ReplayPlayer* find_player(std::uint8_t id) const;
};

// Same explicit little-endian encoding as the wire protocol, and decoded with
// the same hostile-input discipline: a truncated, corrupt or hand-edited file
// yields nullopt rather than a half-built Replay.
std::vector<std::uint8_t> encode_replay(const Replay& replay);
std::optional<Replay> decode_replay(std::span<const std::uint8_t> bytes);

bool write_replay_file(const std::filesystem::path& path, const Replay& replay);
std::optional<Replay> read_replay_file(const std::filesystem::path& path);

// Accumulates a Replay while a match runs. Inert until begin() is called, so
// the server can construct one unconditionally and only arm it when asked.
class ReplayRecorder {
public:
    void begin(std::string map_path, std::uint16_t tick_rate_hz);
    bool recording() const { return recording_; }

    // Players may join mid-match; a repeated id updates the existing entry
    // rather than adding a duplicate.
    void add_player(std::uint8_t id, std::string name, const glm::vec3& spawn);

    // Records one applied input. Commands for the same tick are grouped;
    // ticks must not go backwards (an out-of-order tick is dropped, since
    // replaying it would not reproduce what the server actually did).
    void record(std::uint32_t tick, std::uint8_t player_id, const InputCommand& command);

    const Replay& replay() const { return replay_; }
    std::size_t frame_count() const { return replay_.frames.size(); }

private:
    Replay replay_;
    bool recording_ = false;
};

}  // namespace game
