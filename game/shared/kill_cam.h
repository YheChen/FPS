#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// Killcam support: a short rolling record of where each player was looking,
// so a victim can be shown the seconds leading up to their death from the
// killer's viewpoint.
//
// Why not reuse PositionHistory: that buffer is 32 ticks (~0.5 s) because lag
// compensation never rewinds more than 15, and it stores position only. A
// killcam needs seconds and needs the view angles -- widening the lag-comp
// buffer to serve both would make a hot, correctness-critical path carry
// fields it has no use for.
//
// Why not the replay system: replays store INPUTS, not positions, so that a
// replay landing somewhere different is a determinism bug rather than a replay
// bug. That property is worth more than the killcam, so nothing here touches
// it.
//
// Header-only, pure, unit-tested. The server owns one of these per player.
namespace game {

// 2 seconds at 20 Hz. Long enough to see where the shot came from, short
// enough that eight of these are a rounding error in memory and the whole
// trail fits in one reliable message.
inline constexpr std::size_t kKillCamSamples = 40;

// Sampled at the snapshot rate rather than the tick rate: 60 Hz would triple
// the message for motion nobody can see at playback speed.
inline constexpr std::uint32_t kKillCamTickStride = 3;

struct ViewSample {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

class ViewTrail {
public:
    void push(const ViewSample& sample) {
        samples_[next_] = sample;
        next_ = (next_ + 1) % kKillCamSamples;
        if (count_ < kKillCamSamples) {
            ++count_;
        }
    }

    // Oldest first, so a caller can play it forward without reversing. Returns
    // fewer than asked for when the trail has not filled yet -- a player killed
    // two seconds after spawning has a short story, not an invalid one.
    std::vector<ViewSample> recent(std::size_t count) const {
        const std::size_t take = count < count_ ? count : count_;
        std::vector<ViewSample> out;
        out.reserve(take);
        // Walk back `take` from the write head, wrapping.
        const std::size_t start = (next_ + kKillCamSamples - take) % kKillCamSamples;
        for (std::size_t i = 0; i < take; ++i) {
            out.push_back(samples_[(start + i) % kKillCamSamples]);
        }
        return out;
    }

    std::size_t size() const { return count_; }

    // Called on respawn: the trail is about how someone died, and carrying the
    // previous life into the next one would show a victim footage from before
    // they were even alive.
    void clear() {
        count_ = 0;
        next_ = 0;
    }

private:
    ViewSample samples_[kKillCamSamples]{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

}  // namespace game
