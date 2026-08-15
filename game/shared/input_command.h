#pragma once

#include <cstdint>

// The unit of player intent. Produced by the client once per simulation
// tick, consumed by the local prediction AND (from Milestone 6) by the
// authoritative server. This is the ONLY thing a client may tell the server
// about its player.
namespace game {

enum class Button : std::uint16_t {
    Forward = 1u << 0,
    Back = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    Jump = 1u << 4,
    Fire = 1u << 5,
    Reload = 1u << 6,
    Sprint = 1u << 7,
    Crouch = 1u << 8,
    // Aim down sights (M49). A new bit in a field that was already a u16 on
    // the wire, so this needs no protocol bump: an older server reads the
    // byte it always read and ignores a bit it does not know, and a newer
    // one simply never sees the bit set by an older client.
    Aim = 1u << 9,
};

struct InputCommand {
    std::uint32_t sequence = 0;  // client-assigned, monotonically increasing
    float yaw = 0.0f;            // radians; view direction at the tick
    float pitch = 0.0f;          // radians, clamped to +-89 degrees
    std::uint16_t buttons = 0;   // Button bitfield
    // Desired weapon slot. Sent as state (not an edge event) every tick so a
    // lost packet cannot drop a weapon switch: the server simply converges on
    // the newest slot it has seen.
    std::uint8_t weapon_slot = 0;
};

constexpr bool has_button(const InputCommand& command, Button button) {
    return (command.buttons & static_cast<std::uint16_t>(button)) != 0;
}

constexpr void set_button(InputCommand& command, Button button, bool down) {
    if (down) {
        command.buttons |= static_cast<std::uint16_t>(button);
    } else {
        command.buttons &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(button));
    }
}

}  // namespace game
