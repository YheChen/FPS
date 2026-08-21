#include "game/shared/bot.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

std::optional<std::size_t> nearest_enemy(const glm::vec3& from, Team team,
                                         std::span<const BotTarget> candidates) {
    std::optional<std::size_t> best;
    float nearest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const BotTarget& candidate = candidates[i];
        // Dead players and teammates are both invisible to targeting. A
        // teammate is not "a target worth less" -- it is not a target, and a
        // bot that ranks one at all will pick it whenever it is closer.
        if (!candidate.alive || candidate.team == team) {
            continue;
        }
        const float distance = glm::length(candidate.position - from);
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

BotConfig bot_config_for(BotSkill skill) {
    BotConfig config;  // the Normal defaults
    switch (skill) {
        case BotSkill::Easy:
            config.turn_speed = 2.2f;
            config.reaction_seconds = 0.70f;
            config.aim_error_radians = 0.140f;
            config.aim_drift_hz = 0.40f;
            config.burst_seconds = 0.18f;
            config.burst_pause_seconds = 1.40f;
            break;
        case BotSkill::Normal:
            break;
        case BotSkill::Hard:
            config.turn_speed = 5.5f;
            config.reaction_seconds = 0.16f;
            config.aim_error_radians = 0.030f;
            config.aim_drift_hz = 0.85f;
            config.burst_seconds = 0.40f;
            config.burst_pause_seconds = 0.40f;
            break;
        case BotSkill::Deadly:
            // The old behaviour exactly: instant reaction, perfect aim, and a
            // trigger that is never released.
            config.turn_speed = 8.0f;
            config.reaction_seconds = 0.0f;
            config.aim_error_radians = 0.0f;
            config.fire_cone_radians = 0.10f;
            config.burst_seconds = 1e9f;
            config.burst_pause_seconds = 0.0f;
            break;
    }
    return config;
}

const char* bot_skill_name(BotSkill skill) {
    switch (skill) {
        case BotSkill::Easy:
            return "easy";
        case BotSkill::Normal:
            return "normal";
        case BotSkill::Hard:
            return "hard";
        case BotSkill::Deadly:
            return "deadly";
    }
    return "normal";
}

std::optional<BotSkill> bot_skill_from_name(std::string_view name) {
    if (name == "easy") {
        return BotSkill::Easy;
    }
    if (name == "normal") {
        return BotSkill::Normal;
    }
    if (name == "hard") {
        return BotSkill::Hard;
    }
    if (name == "deadly") {
        return BotSkill::Deadly;
    }
    return std::nullopt;
}

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
        // Fixed for this bot's lifetime, so bots sharing a config sway out of
        // phase with each other.
        state.aim_phase = unit_random(seed, 0xc2b2ae35u) * 2.0f * kPi;
        state.initialized = true;
    }
    state.aim_time += dt;

    InputCommand command;
    command.weapon_slot = config.weapon_slot;

    const float distance =
        senses.has_target ? glm::length(senses.target_position - senses.position) : 0.0f;
    const bool engaging = senses.has_target && distance <= config.sight_range;

    // --- reaction ----------------------------------------------------------
    // A target has to be seen for a while before the bot does anything about
    // it. Held rather than reset while a known target is merely occluded, so
    // ducking behind a pillar for a moment does not buy a fresh reaction
    // delay every time.
    if (!senses.has_target) {
        state.target_seen_seconds = 0.0f;
    } else if (senses.target_visible) {
        state.target_seen_seconds += dt;
    }
    const bool reacted = state.target_seen_seconds >= config.reaction_seconds;

    // --- aim ---------------------------------------------------------------
    // `true_*` is where the target actually is; the bot aims at that plus an
    // error, and its fire gate is judged against the true value. That split is
    // the whole mechanism: the fire cone finally means something, because the
    // aim no longer sits exactly on the target by construction.
    float true_yaw = state.wander_yaw;
    float true_pitch = 0.0f;
    if (engaging) {
        true_yaw = yaw_towards(senses.position, senses.target_position);
        // Aim at chest height rather than at the feet the position refers to.
        const glm::vec3 aim_point = senses.target_position + glm::vec3{0.0f, 1.1f, 0.0f};
        const glm::vec3 eye = senses.position + glm::vec3{0.0f, 1.6f, 0.0f};
        const float horizontal = glm::length(glm::vec2{aim_point.x - eye.x, aim_point.z - eye.z});
        true_pitch = horizontal > 0.01f ? std::atan2(aim_point.y - eye.y, horizontal) : 0.0f;
    }

    // Two incommensurate frequencies, so the sway neither looks like a sine
    // wave nor repeats on a period a player could learn. Weights sum to 1, so
    // the error stays inside +-aim_error_radians.
    float desired_yaw = true_yaw;
    float desired_pitch = true_pitch;
    if (engaging && reacted && config.aim_error_radians > 0.0f) {
        const float w = 2.0f * kPi * config.aim_drift_hz * state.aim_time;
        const float p = state.aim_phase;
        const float sway_yaw = 0.62f * std::sin(w + p) + 0.38f * std::sin(1.73f * w + 2.1f * p);
        const float sway_pitch =
            0.62f * std::sin(0.87f * w + 1.4f * p) + 0.38f * std::sin(1.31f * w + 0.6f * p);
        desired_yaw = true_yaw + config.aim_error_radians * sway_yaw;
        // Vertically a player is a much smaller target than horizontally, so
        // the same angular error here would make bots miss high or low almost
        // always. Halved to keep the sway a challenge rather than a wall.
        desired_pitch = true_pitch + 0.5f * config.aim_error_radians * sway_pitch;
    }

    // Before reacting the bot keeps whatever it was already doing rather than
    // freezing: a target appearing must not stop it mid-stride.
    if (engaging && !reacted) {
        desired_yaw = state.aim_yaw;
        desired_pitch = state.aim_pitch;
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
    // Gated on the aim having caught up with where the bot is TRYING to point
    // -- `desired_*`, which includes its error -- not on where the target
    // actually is.
    //
    // That distinction is the whole difference between an aim-error knob that
    // works and one that does nothing. Gating on the true bearing means the
    // bot only pulls the trigger during the instants its sway happens to be
    // near zero, so every shot it takes is still a good one and more error
    // just means fewer, equally deadly bursts. Gating on its own intent makes
    // it shoot confidently while pointing slightly wrong, which is what
    // missing actually is.
    //
    // Pitch counts too. It never used to, and with a vertical error now in
    // play a bot would otherwise fire over a target's head whenever its yaw
    // was good.
    const bool on_target =
        std::abs(shortest_angle_delta(state.aim_yaw, desired_yaw)) < config.fire_cone_radians &&
        std::abs(state.aim_pitch - desired_pitch) < config.fire_cone_radians;
    const bool wants_to_shoot = engaging && senses.target_visible && reacted && on_target;

    // Trigger discipline. The cycle only advances while the bot wants to
    // shoot, so a pause is not burned through while it is hunting -- and it
    // resets between engagements, so re-acquiring a target does not start
    // mid-pause.
    if (!wants_to_shoot) {
        state.bursting = false;
        state.burst_timer = 0.0f;
    } else {
        state.burst_timer -= dt;
        if (state.burst_timer <= 0.0f) {
            state.bursting = !state.bursting;
            state.burst_timer = state.bursting ? config.burst_seconds : config.burst_pause_seconds;
        }
    }
    if (wants_to_shoot && state.bursting) {
        set_button(command, Button::Fire, true);
    }

    return command;
}

}  // namespace game
