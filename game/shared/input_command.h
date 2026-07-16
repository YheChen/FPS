#pragma once

#include <cstdint>

// The unit of player intent. Produced by the client once per simulation
// tick, consumed by the local prediction AND (from Milestone 6) by the
// authoritative server. This is the ONLY thing a client may tell the server
// about its player.
namespace game {

enum class Button : std::uint8_t {
    Forward = 1u << 0,
    Back = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    Jump = 1u << 4,
    Fire = 1u << 5,
    Reload = 1u << 6,
    Sprint = 1u << 7,
};

struct InputCommand {
    std::uint32_t sequence = 0;  // client-assigned, monotonically increasing
    float yaw = 0.0f;            // radians; view direction at the tick
    float pitch = 0.0f;          // radians, clamped to +-89 degrees
    std::uint8_t buttons = 0;    // Button bitfield
};

constexpr bool has_button(const InputCommand& command, Button button) {
    return (command.buttons & static_cast<std::uint8_t>(button)) != 0;
}

constexpr void set_button(InputCommand& command, Button button, bool down) {
    if (down) {
        command.buttons |= static_cast<std::uint8_t>(button);
    } else {
        command.buttons &= static_cast<std::uint8_t>(~static_cast<std::uint8_t>(button));
    }
}

}  // namespace game
