#pragma once

#include <array>
#include <cstdint>

// Input state as a pure, platform-free state machine. The platform layer
// (Window) translates SDL events into apply_*() calls; game code reads the
// query API. Being SDL-free keeps this unit-testable and usable in headless
// builds.
namespace eng {

// The keys the game actually uses. Extend as needed; kCount stays last.
enum class Key : std::uint8_t {
    W,
    A,
    S,
    D,
    R,
    Space,
    LeftShift,
    Tab,
    Escape,
    Enter,
    F1,
    F2,
    F3,
    kCount,
};

enum class MouseButton : std::uint8_t {
    Left,
    Right,
    kCount,
};

class InputState {
public:
    // Clears per-frame edges (pressed/released) and mouse deltas. Call once
    // per render frame, before feeding events.
    void begin_frame();

    // --- fed by the platform layer -----------------------------------
    void apply_key(Key key, bool down);
    void apply_mouse_button(MouseButton button, bool down);
    void apply_mouse_delta(float dx, float dy);

    // --- queried by game code -----------------------------------------
    bool is_down(Key key) const;
    bool was_pressed(Key key) const;   // edge since begin_frame()
    bool was_released(Key key) const;  // edge since begin_frame()

    bool is_down(MouseButton button) const;
    bool was_pressed(MouseButton button) const;

    // Accumulated relative mouse motion since begin_frame(), in pixels.
    float mouse_dx() const { return mouse_dx_; }
    float mouse_dy() const { return mouse_dy_; }

private:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::kCount);
    static constexpr std::size_t kButtonCount = static_cast<std::size_t>(MouseButton::kCount);

    std::array<bool, kKeyCount> down_{};
    std::array<bool, kKeyCount> pressed_{};
    std::array<bool, kKeyCount> released_{};
    std::array<bool, kButtonCount> button_down_{};
    std::array<bool, kButtonCount> button_pressed_{};
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
};

}  // namespace eng
