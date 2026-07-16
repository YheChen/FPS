#pragma once

#include <glm/glm.hpp>

// Conventions (docs/rendering.md): right-handed, +Y up, -Z forward.
// yaw = 0, pitch = 0 looks down -Z; positive yaw turns right (+X);
// positive pitch looks up. Angles in radians, distances in meters.
namespace eng {

struct Camera {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;

    float fov_y_degrees = 70.0f;
    float aspect = 16.0f / 9.0f;
    float near_plane = 0.05f;
    float far_plane = 300.0f;

    static constexpr float kMaxPitchRadians = glm::radians(89.0f);

    glm::vec3 forward() const;
    glm::vec3 right() const;

    // Clamps pitch to +-89 degrees and wraps yaw into [-pi, pi).
    void clamp_angles();

    glm::mat4 view() const;
    glm::mat4 projection() const;
    glm::mat4 view_projection() const { return projection() * view(); }
};

}  // namespace eng
