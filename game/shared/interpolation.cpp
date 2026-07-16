#include "game/shared/interpolation.h"

#include <cmath>
#include <numbers>

namespace game {

namespace {

// Wrap-aware angle interpolation (shortest arc).
float lerp_angle(float a, float b, float t) {
    const float delta = std::remainder(b - a, 2.0f * std::numbers::pi_v<float>);
    return a + delta * t;
}

}  // namespace

void SnapshotBuffer::push(const RemoteSample& sample) {
    if (!samples_.empty() && sample.tick <= samples_.back().tick) {
        return;
    }
    samples_.push_back(sample);
    // Hard cap: ~2 seconds of snapshots.
    while (samples_.size() > 64) {
        samples_.pop_front();
    }
}

std::optional<RemoteSample> SnapshotBuffer::sample(double tick) const {
    if (samples_.empty()) {
        return std::nullopt;
    }
    if (tick <= static_cast<double>(samples_.front().tick)) {
        return samples_.front();
    }
    if (tick >= static_cast<double>(samples_.back().tick)) {
        return samples_.back();
    }
    // Find the bracketing pair (linear scan; buffer is tiny).
    for (std::size_t i = 0; i + 1 < samples_.size(); ++i) {
        const RemoteSample& a = samples_[i];
        const RemoteSample& b = samples_[i + 1];
        if (tick >= static_cast<double>(a.tick) && tick <= static_cast<double>(b.tick)) {
            const float t = static_cast<float>((tick - a.tick) / double(b.tick - a.tick));
            RemoteSample out;
            out.tick = a.tick;
            out.position = glm::mix(a.position, b.position, t);
            out.yaw = lerp_angle(a.yaw, b.yaw, t);
            out.pitch = lerp_angle(a.pitch, b.pitch, t);
            out.flags = t < 0.5f ? a.flags : b.flags;
            return out;
        }
    }
    return samples_.back();  // unreachable, defensive
}

void SnapshotBuffer::prune_before(std::uint32_t tick) {
    // Keep at least one sample at or before `tick` so it can still bracket.
    while (samples_.size() > 1 && samples_[1].tick <= tick) {
        samples_.pop_front();
    }
}

}  // namespace game
