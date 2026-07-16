#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include <glm/glm.hpp>

// Snapshot interpolation buffer for REMOTE players: stores authoritative
// samples by server tick and produces a smoothly interpolated pose for a
// (fractional) render tick that trails the newest snapshot by the
// interpolation delay. Client-side logic; lives in game/shared for headless
// unit testing.
namespace game {

// How far render time trails the newest snapshot, in server ticks.
// 2 snapshot intervals (2 * 3 ticks) + margin: ~108 ms at 60 Hz.
inline constexpr double kInterpolationDelayTicks = 6.5;

struct RemoteSample {
    std::uint32_t tick = 0;
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::uint8_t flags = 0;
};

class SnapshotBuffer {
public:
    // Samples must arrive with increasing ticks (stale snapshots are dropped
    // upstream); non-increasing pushes are ignored.
    void push(const RemoteSample& sample);

    // Interpolated pose at a fractional tick. Clamps to the oldest/newest
    // sample when outside the buffered range; nullopt only when empty.
    std::optional<RemoteSample> sample(double tick) const;

    // Drops samples older than `tick` (keeps one before it for bracketing).
    void prune_before(std::uint32_t tick);

    bool empty() const { return samples_.empty(); }
    std::size_t size() const { return samples_.size(); }
    std::uint32_t latest_tick() const { return samples_.empty() ? 0 : samples_.back().tick; }

private:
    std::deque<RemoteSample> samples_;
};

}  // namespace game
