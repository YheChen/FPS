#include "engine/platform/input.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using eng::InputState;
using eng::Key;
using eng::MouseButton;

TEST_CASE("key press produces a one-frame edge and a persistent down state", "[input]") {
    InputState input;

    input.begin_frame();
    input.apply_key(Key::W, true);
    CHECK(input.is_down(Key::W));
    CHECK(input.was_pressed(Key::W));

    input.begin_frame();  // next frame: still held, edge gone
    CHECK(input.is_down(Key::W));
    CHECK_FALSE(input.was_pressed(Key::W));

    input.apply_key(Key::W, false);
    CHECK_FALSE(input.is_down(Key::W));
    CHECK(input.was_released(Key::W));
}

TEST_CASE("repeated down events do not retrigger the pressed edge", "[input]") {
    InputState input;
    input.begin_frame();
    input.apply_key(Key::Space, true);
    input.begin_frame();
    input.apply_key(Key::Space, true);  // OS repeat / duplicate
    CHECK_FALSE(input.was_pressed(Key::Space));
    CHECK(input.is_down(Key::Space));
}

TEST_CASE("mouse deltas accumulate within a frame and reset on begin_frame", "[input]") {
    InputState input;
    input.begin_frame();
    input.apply_mouse_delta(3.0f, -1.0f);
    input.apply_mouse_delta(2.0f, 5.0f);
    CHECK(input.mouse_dx() == 5.0f);
    CHECK(input.mouse_dy() == 4.0f);

    input.begin_frame();
    CHECK(input.mouse_dx() == 0.0f);
    CHECK(input.mouse_dy() == 0.0f);
}

TEST_CASE("mouse buttons track down state and pressed edge", "[input]") {
    InputState input;
    input.begin_frame();
    input.apply_mouse_button(MouseButton::Left, true);
    CHECK(input.is_down(MouseButton::Left));
    CHECK(input.was_pressed(MouseButton::Left));

    input.begin_frame();
    CHECK(input.is_down(MouseButton::Left));
    CHECK_FALSE(input.was_pressed(MouseButton::Left));
}

}  // namespace
