#pragma once

#include <memory>

#include <glm/glm.hpp>

namespace eng {

class PhysicsWorld;

struct CharacterConfig {
    float radius = 0.4f;            // capsule radius, meters
    float height = 1.8f;            // total standing height, meters
    float max_slope_degrees = 50.0f;
};

// Kinematic FPS character on Jolt's CharacterVirtual (ADR 0004). The game
// owns and integrates velocity (acceleration, friction, gravity, jumping);
// this class resolves collide-and-slide, stair stepping, and ground
// detection. `position()` is the character's FEET.
//
// Position and velocity are fully settable so client prediction can rewind
// and replay ticks (Milestone 7).
class CharacterController {
public:
    CharacterController(PhysicsWorld& world, const glm::vec3& feet_position,
                        const CharacterConfig& config = {});
    ~CharacterController();
    CharacterController(CharacterController&&) noexcept;
    CharacterController& operator=(CharacterController&&) noexcept;
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    glm::vec3 position() const;
    void set_position(const glm::vec3& feet_position);

    glm::vec3 velocity() const;
    void set_velocity(const glm::vec3& velocity);

    bool on_ground() const;
    glm::vec3 ground_normal() const;

    // Moves by the current velocity for dt seconds, resolving collisions.
    // `gravity` is passed to Jolt for slope/stair handling; velocity
    // integration (including gravity) is the caller's responsibility.
    void update(PhysicsWorld& world, float dt, const glm::vec3& gravity);

    const CharacterConfig& config() const { return config_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    CharacterConfig config_;
};

}  // namespace eng
