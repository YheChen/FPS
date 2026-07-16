#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

// Server-side lag compensation (Milestone 9): the server records each
// player's position per tick; when validating a hitscan shot it rewinds
// victims to where the SHOOTER saw them (their interpolated render tick),
// bounded to kMaxRewindTicks so a hostile client cannot claim to live in
// the distant past. Header-only, pure, unit-tested.
namespace game {

// ~250 ms at 60 Hz. Also the upper bound on how far a client's claimed
// view tick may trail the current server tick.
inline constexpr std::uint32_t kMaxRewindTicks = 15;

class PositionHistory {
public:
    void push(std::uint32_t tick, const glm::vec3& position) {
        entries_[tick % kCapacity] = {tick, position, true};
        latest_tick_ = tick;
    }

    // Exact position at `tick`, if still buffered.
    std::optional<glm::vec3> at(std::uint32_t tick) const {
        const Entry& entry = entries_[tick % kCapacity];
        if (entry.valid && entry.tick == tick) {
            return entry.position;
        }
        return std::nullopt;
    }

    std::uint32_t latest_tick() const { return latest_tick_; }

private:
    static constexpr std::size_t kCapacity = 32;  // > kMaxRewindTicks

    struct Entry {
        std::uint32_t tick = 0;
        glm::vec3 position{0.0f};
        bool valid = false;
    };
    std::array<Entry, kCapacity> entries_{};
    std::uint32_t latest_tick_ = 0;
};

// Clamps a client-claimed view tick into the legal rewind window
// [current - kMaxRewindTicks, current]. A claim of 0 (client has no
// estimate yet) or a future tick resolves to `current_tick`.
inline std::uint32_t clamp_rewind_tick(std::uint32_t claimed_view_tick,
                                       std::uint32_t current_tick) {
    if (claimed_view_tick == 0 || claimed_view_tick > current_tick) {
        return current_tick;
    }
    const std::uint32_t oldest =
        current_tick > kMaxRewindTicks ? current_tick - kMaxRewindTicks : 0;
    return claimed_view_tick < oldest ? oldest : claimed_view_tick;
}

}  // namespace game
