#pragma once

#include <cstdint>

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
    float turn_speed = 5.0f;           // radians/sec toward the aim point
    float sight_range = 45.0f;         // metres
    float fire_cone_radians = 0.10f;   // only shoots when aim is this close
    float preferred_range = 11.0f;     // backs off when nearer than this
    float engage_range = 26.0f;        // closes when further than this
    float strafe_seconds = 1.6f;       // between strafe direction flips
    float wander_seconds = 2.8f;       // between idle heading changes
    float jump_chance = 0.010f;        // per tick while engaging
    float wall_avoid_distance = 2.2f;  // turns away inside this
};

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
