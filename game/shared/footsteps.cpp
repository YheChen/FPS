#include "game/shared/footsteps.h"

#include <algorithm>
#include <cmath>

#include "game/shared/player_movement.h"

namespace game {

namespace {

// A local hash, deliberately not game/shared/rng.h, for the same reason
// particle_sim.cpp has its own: gameplay randomness has to stay bit-exact
// across client and server, and nothing cosmetic should be able to reach into
// it. Nothing here is ever simulated twice, so it does not need to be
// reproducible -- only unpatterned.
std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// -1..1 from a seed.
float hash_signed(std::uint32_t seed) {
    return static_cast<float>(hash_u32(seed) >> 8) / static_cast<float>(1u << 23) - 1.0f;
}

// Piecewise-linear against speed normalised to the walking top speed, so the
// curve follows the movement config rather than a number typed next to it.
float gait_gain(float speed, bool crouching, const FootstepConfig& config) {
    if (crouching) {
        return config.crouch_gain;
    }
    const float relative = speed / kMove.max_speed;
    if (relative <= config.creep_fraction) {
        return config.creep_gain;
    }
    if (relative < 1.0f) {
        const float t = (relative - config.creep_fraction) / (1.0f - config.creep_fraction);
        return glm::mix(config.creep_gain, config.run_gain, t);
    }
    // Sprint is a real multiplier on top speed, so the loud end of the ramp
    // sits exactly where sprinting actually gets you and not a metre further.
    const float t = glm::clamp((relative - 1.0f) / (kMove.sprint_multiplier - 1.0f), 0.0f, 1.0f);
    return glm::mix(config.run_gain, config.sprint_gain, t);
}

}  // namespace

FootstepEvent update_footsteps(FootstepState& state, const glm::vec3& position,
                               const glm::vec3& velocity, bool on_ground, bool crouching,
                               const FootstepConfig& config) {
    FootstepEvent event;

    const glm::vec3 delta = position - state.last_position;
    const bool first_sight = !state.tracking;
    state.last_position = position;
    state.tracking = true;
    if (first_sight || glm::length(delta) > config.teleport_meters) {
        // Nobody walked anywhere: they appeared, or they were moved. Adopt the
        // new position silently. Without this a respawn across the arena is
        // one forty-metre stride, and the whole map hears a burst of steps.
        state.distance = 0.0f;
        state.fall_speed = 0.0f;
        state.was_on_ground = on_ground;
        return event;
    }

    if (!on_ground) {
        // Airborne: no strides, but remember the worst fall so the landing
        // knows how hard it was. Reading it at touchdown is too late -- the
        // character controller has already cancelled the downward velocity by
        // the time on_ground comes back.
        state.fall_speed = std::max(state.fall_speed, -velocity.y);
        state.was_on_ground = false;
        return event;
    }

    if (!state.was_on_ground) {
        state.was_on_ground = true;
        // A landing IS a foot hitting the ground, so it restarts the stride
        // rather than sitting on top of it: the next step comes a full stride
        // later instead of at whatever fraction was left over at take-off.
        state.distance = 0.0f;
        const float fall = state.fall_speed;
        state.fall_speed = 0.0f;
        if (fall > config.land_floor_speed) {
            const float span = std::max(0.01f, config.land_full_speed - config.land_floor_speed);
            const float t = glm::clamp((fall - config.land_floor_speed) / span, 0.0f, 1.0f);
            event.landed = true;
            // Crouching does not quieten this. Weight arriving from a height
            // is weight arriving from a height, and letting a held crouch key
            // silence it would turn crouch-jumping into free silent traversal
            // -- the opposite of the trade the stance is meant to be.
            event.land_gain = glm::mix(config.land_min_gain, config.land_max_gain, t);
        }
        return event;
    }

    state.distance += glm::length(glm::vec2{delta.x, delta.z});
    if (state.distance < config.stride_meters) {
        return event;
    }
    // Keep the remainder rather than zeroing, so cadence stays locked to
    // ground covered instead of stretching by up to a frame of travel every
    // step. fmod rather than one subtraction so a frame hitch long enough to
    // cover several strides costs one step, not a burst of them.
    state.distance = std::fmod(state.distance, config.stride_meters);

    const std::uint32_t roll = ++state.counter;
    // Never the same sample twice running: an immediate repeat is the single
    // most audible way a four-sample set gives itself away. Offsetting by
    // 1..3 also means the order never settles into a loop the ear can learn.
    state.variant =
        (state.variant + 1 + hash_u32(roll) % (kFootstepVariants - 1)) % kFootstepVariants;

    event.stepped = true;
    event.variant = state.variant;
    event.gain = gait_gain(glm::length(glm::vec2{velocity.x, velocity.z}), crouching, config) *
                 (1.0f + config.gain_jitter * hash_signed(roll ^ 0x9e3779b9u));
    event.pitch = 1.0f + config.pitch_jitter * hash_signed(roll ^ 0x85ebca6bu);
    return event;
}

}  // namespace game
