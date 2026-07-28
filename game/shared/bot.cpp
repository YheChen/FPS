#include "game/shared/bot.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "game/shared/rng.h"

namespace game {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Bots use the gameplay RNG deliberately, unlike particles: their decisions
// feed advance_player, so they are part of the simulation and must be
// reproducible for replays to work.
float unit_random(std::uint32_t seed, std::uint32_t salt) {
    return hash_float01(hash_combine(seed, salt));
}

}  // namespace

float yaw_towards(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 delta = to - from;
    // Matches view_direction(): x = sin(yaw), z = -cos(yaw).
    return std::atan2(delta.x, -delta.z);
}

float shortest_angle_delta(float current, float target) {
    float delta = std::fmod(target - current + kPi, 2.0f * kPi);
    if (delta < 0.0f) {
        delta += 2.0f * kPi;
    }
    return delta - kPi;
}

InputCommand decide(BotState& state, const BotSenses& senses, const BotConfig& config, float dt,
                    std::uint32_t seed) {
    if (!state.initialized) {
        state.aim_yaw = senses.yaw;
        state.wander_yaw = senses.yaw;
        state.initialized = true;
    }

    InputCommand command;
    command.weapon_slot = 0;

    // --- aim ---------------------------------------------------------------
    float desired_yaw = state.wander_yaw;
    float desired_pitch = 0.0f;
    const float distance =
        senses.has_target ? glm::length(senses.target_position - senses.position) : 0.0f;

    if (senses.has_target && distance <= config.sight_range) {
        desired_yaw = yaw_towards(senses.position, senses.target_position);
        // Aim at chest height rather than at the feet the position refers to.
        const glm::vec3 aim_point = senses.target_position + glm::vec3{0.0f, 1.1f, 0.0f};
        const glm::vec3 eye = senses.position + glm::vec3{0.0f, 1.6f, 0.0f};
        const float horizontal = glm::length(glm::vec2{aim_point.x - eye.x, aim_point.z - eye.z});
        desired_pitch = horizontal > 0.01f ? std::atan2(aim_point.y - eye.y, horizontal) : 0.0f;
    }

    // Turn toward the aim point at a bounded rate. Snapping instantly would
    // make bots inhumanly accurate and would also skip the whole point of
    // having a fire cone.
    const float yaw_delta = shortest_angle_delta(state.aim_yaw, desired_yaw);
    const float max_turn = config.turn_speed * dt;
    state.aim_yaw += std::clamp(yaw_delta, -max_turn, max_turn);
    state.aim_pitch += std::clamp(desired_pitch - state.aim_pitch, -max_turn, max_turn);
    state.aim_pitch = std::clamp(state.aim_pitch, -1.5f, 1.5f);

    command.yaw = state.aim_yaw;
    command.pitch = state.aim_pitch;

    // --- movement ----------------------------------------------------------
    state.strafe_timer -= dt;
    if (state.strafe_timer <= 0.0f) {
        state.strafe_timer = config.strafe_seconds;
        state.strafe_sign = unit_random(seed, 0x51ed270bu) < 0.5f ? -1.0f : 1.0f;
    }

    if (senses.has_target) {
        // Circle-strafe, closing or backing off to hold a working range.
        set_button(command, state.strafe_sign > 0.0f ? Button::Right : Button::Left, true);
        if (distance > config.engage_range) {
            set_button(command, Button::Forward, true);
            set_button(command, Button::Sprint, true);
        } else if (distance < config.preferred_range) {
            set_button(command, Button::Back, true);
        }
        if (senses.on_ground && unit_random(seed, 0x9e3779b9u) < config.jump_chance) {
            set_button(command, Button::Jump, true);
        }
    } else {
        // Idle: pick a heading every so often and walk it.
        state.wander_timer -= dt;
        if (state.wander_timer <= 0.0f) {
            state.wander_timer = config.wander_seconds;
            state.wander_yaw = (unit_random(seed, 0x85ebca6bu) * 2.0f - 1.0f) * kPi;
        }
        set_button(command, Button::Forward, true);
    }

    // Walls win over whatever the plan was: a bot grinding into a corner for
    // the rest of the match is the most obvious way this can look broken.
    if (senses.forward_clearance < config.wall_avoid_distance) {
        set_button(command, Button::Forward, false);
        set_button(command, Button::Back, true);
        state.wander_yaw = state.aim_yaw + (state.strafe_sign > 0.0f ? 1.2f : -1.2f);
        state.wander_timer = config.wander_seconds;
    }

    // --- shooting ----------------------------------------------------------
    // Only when the target is actually visible and the aim has caught up,
    // so bots do not fire through cover or spray while spinning.
    if (senses.has_target && senses.target_visible && distance <= config.sight_range &&
        std::abs(shortest_angle_delta(state.aim_yaw, desired_yaw)) < config.fire_cone_radians) {
        set_button(command, Button::Fire, true);
    }

    return command;
}

}  // namespace game
