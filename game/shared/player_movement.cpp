#include "game/shared/player_movement.h"

#include <algorithm>
#include <cmath>

namespace game {

namespace {

glm::vec2 wish_direction(const InputCommand& command) {
    glm::vec2 axis{0.0f};
    if (has_button(command, Button::Forward)) {
        axis.y += 1.0f;
    }
    if (has_button(command, Button::Back)) {
        axis.y -= 1.0f;
    }
    if (has_button(command, Button::Right)) {
        axis.x += 1.0f;
    }
    if (has_button(command, Button::Left)) {
        axis.x -= 1.0f;
    }
    if (glm::dot(axis, axis) > 0.0f) {
        axis = glm::normalize(axis);
    }
    // Rotate into world space by yaw. Camera convention: yaw=0 faces -Z.
    const float s = std::sin(command.yaw);
    const float c = std::cos(command.yaw);
    const glm::vec2 forward{s, -c};  // (x, z)
    const glm::vec2 right{c, s};
    return forward * axis.y + right * axis.x;
}

}  // namespace

void advance_player(PlayerState& state, const InputCommand& command, float dt,
                    eng::CharacterController& controller, eng::PhysicsWorld& world,
                    const MoveConfig& config) {
    glm::vec3 velocity = state.velocity;
    glm::vec2 horizontal{velocity.x, velocity.z};

    // Ground friction (exponential damp), then acceleration toward wish dir.
    if (state.on_ground) {
        horizontal *= std::max(0.0f, 1.0f - config.friction * dt);
    }
    const glm::vec2 wish = wish_direction(command);
    const float accel = state.on_ground ? config.ground_accel : config.air_accel;
    horizontal += wish * accel * dt;

    const float speed = glm::length(horizontal);
    if (speed > config.max_speed) {
        horizontal *= config.max_speed / speed;
    }

    // Vertical: jump / gravity. A small downward bias while grounded keeps
    // the character glued to ramps and stairs.
    float vertical = velocity.y;
    if (state.on_ground) {
        vertical = has_button(command, Button::Jump) ? config.jump_speed : -0.5f;
    } else {
        vertical -= config.gravity * dt;
    }

    velocity = {horizontal.x, vertical, horizontal.y};

    // Sync the collision proxy to the authoritative state, then step it.
    // refresh_ground makes the step a pure function of (position, velocity):
    // without it, prediction replay would see stale internal contact state.
    controller.set_position(state.position);
    controller.set_velocity(velocity);
    controller.refresh_ground(world);
    controller.update(world, dt, {0.0f, -config.gravity, 0.0f});

    state.position = controller.position();
    state.velocity = controller.velocity();
    state.on_ground = controller.on_ground();
}

}  // namespace game
