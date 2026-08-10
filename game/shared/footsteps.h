#pragma once

#include <cstdint>

#include <glm/glm.hpp>

// Footsteps and landings: when a figure's foot hits the floor, and how loud.
//
// Steps are fired from DISTANCE TRAVELLED, never from a timer. A timer is a
// cadence that happens to be near the right one at exactly one speed and
// wrong everywhere else -- it keeps ticking while a player is stopped against
// a wall, and it drifts against the run animation the moment anyone strafes.
// Distance cannot: crossing the arena costs a fixed number of steps whatever
// route or speed took you there, which is what makes the sound a reliable
// read on how far someone has moved.
//
// Cosmetic, and render-clock only. This reads position and velocity and never
// writes to them, exactly like particles and the character animation; the
// fixed-tick simulation must stay bit-exact for prediction and the M17 replay
// system. Hence also the local hash inside footsteps.cpp instead of
// game/shared/rng.h.
//
// Client-side logic; it lives in game/shared so it can be unit tested
// headlessly, the same reason interpolation.h does.
namespace game {

inline constexpr std::uint32_t kFootstepVariants = 4;

struct FootstepConfig {
    // Horizontal metres between footfalls.
    //
    // The run clip is 0.8 s long and contains two footfalls, and the client
    // plays it at speed/3, so the ANIMATION's implied stride is a fixed 1.2 m
    // at any speed above 3 m/s. Firing a sound on every one of those would be
    // five steps a second at the 6 m/s move speed in player_movement.h, which
    // is past the rate at which the ear stops counting events and starts
    // hearing a texture -- and a footstep that cannot be counted carries no
    // information about how fast someone is closing.
    //
    // 2.4 m is exactly two of the animation's footfalls, so every sound still
    // lands ON a footfall (in phase with the feet, never sliding against
    // them) at half the cadence: 2.5 steps/s at the 6.0 m/s move speed,
    // 3.9 at the 9.3 m/s sprint, and a step every ~1.15 s at a 2.7 m/s
    // crouch. Below 3 m/s the animation clamps its playback rate and the two
    // do drift apart, which is correct: at a crawl the feet slide in the clip
    // and distance is the honest source.
    float stride_meters = 2.4f;

    // Gain by gait, against speed normalised to the walking top speed
    // (1.0 = flat out, ~1.55 = sprinting). Volume is the only channel a
    // listener has for "how committed is this person to moving", so the
    // stealth/speed trade has to be visible here or it does not exist.
    float creep_fraction = 0.4f;  // at or below this, a step is as quiet as it gets
    float creep_gain = 0.30f;
    float run_gain = 0.55f;
    float sprint_gain = 0.95f;
    // Crouching. Not silent: a crouching enemy inside a few metres should
    // still be a faint scuff, or crouch becomes a cloak rather than a trade.
    // At this level the engine's distance falloff has it inaudible past about
    // ten metres, so it costs the crouching player nothing at range and
    // rewards a defender who is genuinely close.
    float crouch_gain = 0.18f;

    // Landing. Below the floor there is no sound at all: with gravity at
    // 20 m/s^2, 4 m/s is a 0.4 m drop, so stepping off a kerb or clearing a
    // ledge stays silent and only a real fall thuds. A full jump lands at the
    // 7 m/s it took off with, comfortably inside the ramp.
    float land_floor_speed = 4.0f;
    float land_full_speed = 11.0f;
    float land_min_gain = 0.35f;
    float land_max_gain = 1.0f;

    // Per-step variation, so a run cycle is not one sample machine-gunned.
    // Small on purpose: this is the same boots on the same floor, and
    // anything wider stops sounding like one person.
    float gain_jitter = 0.10f;    // +/- fraction
    float pitch_jitter = 0.045f;  // +/- fraction

    // A frame's movement longer than this is not movement, it is a respawn,
    // a map change or a reconciliation snap. Sprinting covers 0.16 m in a
    // 60 fps frame, so nothing a player can do reaches it.
    float teleport_meters = 2.0f;
};

inline constexpr FootstepConfig kFootsteps{};

// Per-figure accumulator. One of these per player the client can hear,
// including the local one.
struct FootstepState {
    glm::vec3 last_position{0.0f};
    float distance = 0.0f;    // metres walked since the last footfall
    float fall_speed = 0.0f;  // worst downward speed seen while airborne
    std::uint32_t variant = 0;
    std::uint32_t counter = 0;
    bool tracking = false;  // false until the first update seeds last_position
    bool was_on_ground = true;
};

// What to play this frame. Both flags can be false, and are on almost every
// frame; a step and a landing never come together, because a landing already
// is a foot hitting the ground.
struct FootstepEvent {
    bool stepped = false;
    bool landed = false;
    std::uint32_t variant = 0;  // which of the kFootstepVariants samples
    float gain = 0.0f;          // step gain, 0..1
    float pitch = 1.0f;         // playback rate multiplier
    float land_gain = 0.0f;
};

// Advances one figure by one RENDERED frame. `velocity` sets the gain and the
// landing weight; the distance walked comes from the change in `position`, so
// a remote player interpolated from snapshots and a locally predicted one go
// through the same path and neither can drift.
FootstepEvent update_footsteps(FootstepState& state, const glm::vec3& position,
                               const glm::vec3& velocity, bool on_ground, bool crouching,
                               const FootstepConfig& config = kFootsteps);

}  // namespace game
