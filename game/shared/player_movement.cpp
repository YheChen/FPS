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

// Local forward component of the wish direction, in [-1, 1]. Sprinting is
// only allowed when actually pushing forward.
float forward_input(const InputCommand& command) {
    float axis = 0.0f;
    if (has_button(command, Button::Forward)) {
        axis += 1.0f;
    }
    if (has_button(command, Button::Back)) {
        axis -= 1.0f;
    }
    return axis;
}

}  // namespace

bool has_standing_headroom(const glm::vec3& feet, eng::PhysicsWorld& world,
                           const MoveConfig& config) {
    // Cast from just above the crouched head up to standing height. Anything
    // hit means a ceiling is in the way.
    const float start = config.crouch_height * 0.9f;
    const float needed = config.stand_height + config.stand_clearance_epsilon - start;
    const glm::vec3 origin = feet + glm::vec3{0.0f, start, 0.0f};
    return !world.raycast(origin, {0.0f, 1.0f, 0.0f}, needed).has_value();
}

void advance_player(PlayerState& state, const InputCommand& command, float dt,
                    eng::CharacterController& controller, eng::PhysicsWorld& world,
                    const MoveConfig& config) {
    // --- stance -------------------------------------------------------
    // Crouching is immediate; standing back up requires headroom, so a player
    // cannot stand inside geometry they crouched under.
    const bool wants_crouch = has_button(command, Button::Crouch);
    if (wants_crouch) {
        state.crouching = true;
    } else if (state.crouching && has_standing_headroom(state.position, world, config)) {
        state.crouching = false;
    }
    controller.set_crouched(world, state.crouching);

    // Sprint: grounded, not crouching, and pushing forward.
    state.sprinting = has_button(command, Button::Sprint) && state.on_ground && !state.crouching &&
                      forward_input(command) > 0.0f;

    float max_speed = config.max_speed;
    if (state.crouching) {
        max_speed *= config.crouch_multiplier;
    } else if (state.sprinting) {
        max_speed *= config.sprint_multiplier;
    }

    glm::vec3 velocity = state.velocity;
    glm::vec2 horizontal{velocity.x, velocity.z};

    // Ground friction (exponential damp), then acceleration toward wish dir.
    if (state.on_ground) {
        horizontal *= std::max(0.0f, 1.0f - config.friction * dt);
    }
    // Accelerate toward the wish direction, but only up to the target speed:
    // the step is capped both by the acceleration rate and by the remaining
    // gap, so we converge on exactly max_speed for the current stance.
    const glm::vec2 wish = wish_direction(command);
    const float accel = state.on_ground ? config.ground_accel : config.air_accel;
    const float current_along_wish = glm::dot(horizontal, wish);
    const float gap = max_speed - current_along_wish;
    if (gap > 0.0f) {
        horizontal += wish * std::min(accel * max_speed * dt, gap);
    }

    // Hard ceiling as well, in case an external impulse overshot it.
    const float speed = glm::length(horizontal);
    if (speed > max_speed) {
        horizontal *= max_speed / speed;
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
