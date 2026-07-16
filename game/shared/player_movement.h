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
    float max_speed = 6.0f;       // m/s horizontal
    float ground_accel = 60.0f;   // m/s^2
    float air_accel = 12.0f;      // m/s^2
    float friction = 10.0f;       // 1/s exponential horizontal damping on ground
    float gravity = 20.0f;        // m/s^2 (snappier than 9.81 on purpose)
    float jump_speed = 7.0f;      // m/s -> apex ~1.2m
    float eye_height = 1.62f;     // camera above feet
};

inline constexpr MoveConfig kMove{};

struct PlayerState {
    glm::vec3 position{0.0f};  // feet
    glm::vec3 velocity{0.0f};
    bool on_ground = false;
};

// Advances one fixed tick. The controller is used as scratch collision
// state: its position/velocity are overwritten from `state` first.
void advance_player(PlayerState& state, const InputCommand& command, float dt,
                    eng::CharacterController& controller, eng::PhysicsWorld& world,
                    const MoveConfig& config = kMove);

}  // namespace game
