#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/glm.hpp>

#include "game/shared/input_command.h"

// Bot decision-making.
//
// A bot is not a special kind of entity: it is a player whose InputCommands
// are synthesized instead of received. It occupies a normal player slot, so
// movement, hit detection, scoring, snapshots and replay recording all treat
// it exactly like a human. That is deliberate -- a parallel "bot movement"
// path would stop being a test of the real simulation, and would drift.
//
// `decide` is a pure function of (state, senses, config, seed). It never
// touches the physics world, so the whole decision layer is unit-testable
// without a map, a server or a GPU. Gathering the senses is the server's job.
namespace game {

struct BotConfig {
    float turn_speed = 3.2f;           // radians/sec toward the aim point
    float sight_range = 45.0f;         // metres
    float fire_cone_radians = 0.06f;   // only shoots when aim is this close
    float preferred_range = 11.0f;     // backs off when nearer than this
    float engage_range = 26.0f;        // closes when further than this
    float strafe_seconds = 1.6f;       // between strafe direction flips
    float wander_seconds = 2.8f;       // between idle heading changes
    float jump_chance = 0.010f;        // per tick while engaging
    float wall_avoid_distance = 2.2f;  // turns away inside this

    // Which gun every bot carries. Not a difficulty knob -- it exists so an
    // automated run can put a weapon other than the rifle in front of the
    // camera. Bots were pinned to slot 0, so verifying anything about the
    // smg, shotgun, sniper or knife in a real match meant editing a .cfg and
    // remembering to revert it. Out-of-range clamps to 0 in update_loadout,
    // the same as any other slot a command can carry.
    std::uint8_t weapon_slot = 0;

    // --- the three things that make a bot beatable -----------------------
    //
    // Without these a bot's aim converges on its target and then STAYS there,
    // exactly, forever, however the target moves. `turn_speed` bounds how
    // fast it gets on target; nothing bounded how well it held. Steady-state
    // error was zero, which is not a hard opponent so much as an impossible
    // one.

    // Seconds a target must be continuously visible before the bot reacts to
    // it at all -- no turning, no firing. Human reaction to an unexpected
    // visual stimulus is roughly 0.2-0.3 s, and a bot without any is the
    // single most inhuman thing about it.
    float reaction_seconds = 0.40f;

    // Peak angular aim error, radians. This is an ANGLE, so the linear miss
    // it produces grows with distance -- which is what makes long shots
    // genuinely unreliable while a point-blank fight stays deadly. 0.085 rad
    // is about 1.7 m of sway at 20 m, against a capsule 0.4 m in radius.
    float aim_error_radians = 0.085f;

    // How fast that error wanders. Slow on purpose: fast jitter would average
    // back onto the target across a burst and read as a twitch, where a slow
    // sway makes whole bursts miss the way a real player's do.
    float aim_drift_hz = 0.55f;

    // Trigger discipline, and the one that actually decides how lethal a bot
    // is. The rifle fires 600 rpm for 25 damage against 100 health, so a held
    // trigger kills in 0.4 s of hits -- at which point aim error only changes
    // how long "a moment" is, not the outcome. A bot that held the trigger
    // from first sight until someone died was never really an aiming problem.
    //
    // These also give a human the thing they actually need, which is a gap:
    // time to break line of sight, close, or shoot back.
    float burst_seconds = 0.24f;        // ~2-3 rounds from the rifle
    float burst_pause_seconds = 0.90f;  // then off the trigger
};

// Presets, so difficulty is a launch flag rather than a recompile.
//
// `Deadly` is the pre-existing behaviour -- zero reaction time, zero aim
// error -- kept because it is the useful control case when measuring whether
// a change to movement or hit detection made bots better or worse.
enum class BotSkill : std::uint8_t { Easy, Normal, Hard, Deadly };

BotConfig bot_config_for(BotSkill skill);
const char* bot_skill_name(BotSkill skill);
std::optional<BotSkill> bot_skill_from_name(std::string_view name);

// Everything a bot can perceive this tick. Explicit rather than a pointer to
// the world, so `decide` stays pure and a test can hand it any situation.
struct BotSenses {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    bool on_ground = false;

    bool has_target = false;
    glm::vec3 target_position{0.0f};
    // False when the target is known but behind cover: the bot keeps moving
    // toward its last seen position without shooting a wall.
    bool target_visible = false;

    // Distance to the nearest obstacle straight ahead. Large means clear.
    float forward_clearance = 1000.0f;
};

// Carried between ticks. Kept separate from the senses so the decision
// function can stay a pure transformation of the two.
struct BotState {
    float strafe_timer = 0.0f;
    float wander_timer = 0.0f;
    float strafe_sign = 1.0f;
    float wander_yaw = 0.0f;
    float aim_yaw = 0.0f;
    float aim_pitch = 0.0f;
    // Accumulated seconds, driving the aim wander. A continuous time base is
    // what lets the error be smooth; the per-tick `seed` cannot provide one.
    float aim_time = 0.0f;
    // Fixed once per bot, so two bots sharing a config do not sway in
    // lockstep and read as one opponent duplicated.
    float aim_phase = 0.0f;
    // How long the current target has been continuously visible, for the
    // reaction delay. Held (not reset) while a known target is behind cover:
    // the bot already knows it is there, and re-reacting on every flicker of
    // line-of-sight would be its own artefact.
    float target_seen_seconds = 0.0f;
    // Trigger discipline: seconds left in the current burst or pause.
    float burst_timer = 0.0f;
    bool bursting = false;
    bool initialized = false;
};

// Yaw that points from `from` toward `to`, in the project's convention
// (yaw 0 looks down -Z, positive yaw turns toward +X).
float yaw_towards(const glm::vec3& from, const glm::vec3& to);

// Shortest signed angular difference from `current` to `target`, in
// (-pi, pi]. Turning the long way round is the classic bug here.
float shortest_angle_delta(float current, float target);

// Produces this tick's command. `seed` should vary per (tick, bot) and is
// hashed internally: the randomness is deterministic, so a recorded match
// containing bots replays exactly like any other.
InputCommand decide(BotState& state, const BotSenses& senses, const BotConfig& config, float dt,
                    std::uint32_t seed);

}  // namespace game
