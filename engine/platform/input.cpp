#include "engine/platform/input.h"

namespace eng {

namespace {

template <typename E>
std::size_t index_of(E value) {
    return static_cast<std::size_t>(value);
}

}  // namespace

void InputState::begin_frame() {
    pressed_.fill(false);
    released_.fill(false);
    button_pressed_.fill(false);
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
}

void InputState::apply_key(Key key, bool down) {
    if (key >= Key::kCount) {
        return;
    }
    const std::size_t i = index_of(key);
    if (down && !down_[i]) {
        pressed_[i] = true;
    }
    if (!down && down_[i]) {
        released_[i] = true;
    }
    down_[i] = down;
}

void InputState::apply_mouse_button(MouseButton button, bool down) {
    if (button >= MouseButton::kCount) {
        return;
    }
    const std::size_t i = index_of(button);
    if (down && !button_down_[i]) {
        button_pressed_[i] = true;
    }
    button_down_[i] = down;
}

void InputState::apply_mouse_delta(float dx, float dy) {
    mouse_dx_ += dx;
    mouse_dy_ += dy;
}

bool InputState::is_down(Key key) const {
    return key < Key::kCount && down_[index_of(key)];
}

bool InputState::was_pressed(Key key) const {
    return key < Key::kCount && pressed_[index_of(key)];
}

bool InputState::was_released(Key key) const {
    return key < Key::kCount && released_[index_of(key)];
}

bool InputState::is_down(MouseButton button) const {
    return button < MouseButton::kCount && button_down_[index_of(button)];
}

bool InputState::was_pressed(MouseButton button) const {
    return button < MouseButton::kCount && button_pressed_[index_of(button)];
}

}  // namespace eng
