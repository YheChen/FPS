#pragma once

#include <glm/glm.hpp>

#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
#include "game/shared/input_command.h"

// FPS movement simulation, shared verbatim by client prediction and the
// authoritative server (ADR 0002/0004). advance_player is a pure function
// of (state, input, dt) plus collision queries: it writes the controller's
// position/velocity from `state` before stepping, so it can replay
// arbitrary historical states (reconciliation).
namespace game {

inline constexpr double kTickRate = 60.0;
inline constexpr float kTickSeconds = 1.0f / 60.0f;

struct MoveConfig {
    float max_speed = 6.0f;  // m/s horizontal, walking
    // Acceleration coefficients (1/s), Quake-style: the per-tick speed gain
    // is accel * target_speed * dt, and only ever closes the gap to the
    // target. Scaling with the target is what makes sprint actually faster -
    // with a fixed m/s^2 acceleration, friction alone would pin the terminal
    // speed to the same value no matter how high the cap was raised.
    float ground_accel = 10.0f;
    float air_accel = 2.0f;
    float friction = 10.0f;    // 1/s exponential horizontal damping on ground
    float gravity = 20.0f;     // m/s^2 (snappier than 9.81 on purpose)
    float jump_speed = 7.0f;   // m/s -> apex ~1.2m
    float eye_height = 1.62f;  // camera above feet, standing

    // Sprint multiplies top speed but only while grounded and pushing
    // forward, so it cannot be used to strafe-dodge faster.
    float sprint_multiplier = 1.55f;
    // Crouch trades speed for a smaller silhouette.
    float crouch_multiplier = 0.45f;
    float crouch_eye_height = 1.0f;
    // Total capsule heights; must match CharacterConfig.
    float stand_height = 1.8f;
    float crouch_height = 1.15f;
    // A little slack so a player can always stand where they fit exactly.
    float stand_clearance_epsilon = 0.05f;
};

inline constexpr MoveConfig kMove{};

struct PlayerState {
    glm::vec3 position{0.0f};  // feet
    glm::vec3 velocity{0.0f};
    bool on_ground = false;
    bool crouching = false;
    // True only while the sprint conditions were actually met this tick;
    // used for HUD/FOV feedback and future stamina.
    bool sprinting = false;
};

// Eye height for the player's current stance (used for the camera and as the
// hitscan origin, so client view and server rays agree).
constexpr float eye_height_for(const PlayerState& state, const MoveConfig& config = kMove) {
    return state.crouching ? config.crouch_eye_height : config.eye_height;
}

// True if there is room to stand up at `position` (nothing within standing
// height above the head). Deterministic: a single upward ray.
bool has_standing_headroom(const glm::vec3& feet, eng::PhysicsWorld& world,
                           const MoveConfig& config = kMove);

// Advances one fixed tick. The controller is used as scratch collision
// state: its position/velocity/stance are overwritten from `state` first.
void advance_player(PlayerState& state, const InputCommand& command, float dt,
                    eng::CharacterController& controller, eng::PhysicsWorld& world,
                    const MoveConfig& config = kMove);

}  // namespace game
