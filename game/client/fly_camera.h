#pragma once

#include "engine/platform/input.h"
#include "engine/rendering/camera.h"

namespace game {

// Free-flying debug camera. Replaced by the FPS character controller from
// Milestone 4 onward (kept for spectating/debugging).
struct FlyCamera {
    eng::Camera camera;
    float move_speed = 8.0f;          // m/s
    float look_sensitivity = 0.002f;  // radians per pixel

    void update(const eng::InputState& input, float dt, bool mouse_captured) {
        if (mouse_captured) {
            camera.yaw += input.mouse_dx() * look_sensitivity;
            camera.pitch -= input.mouse_dy() * look_sensitivity;
            camera.clamp_angles();
        }

        glm::vec3 wish{0.0f};
        const glm::vec3 forward = camera.forward();
        const glm::vec3 right = camera.right();
        if (input.is_down(eng::Key::W)) {
            wish += forward;
        }
        if (input.is_down(eng::Key::S)) {
            wish -= forward;
        }
        if (input.is_down(eng::Key::D)) {
            wish += right;
        }
        if (input.is_down(eng::Key::A)) {
            wish -= right;
        }
        if (input.is_down(eng::Key::Space)) {
            wish += glm::vec3{0.0f, 1.0f, 0.0f};
        }
        if (input.is_down(eng::Key::LeftShift)) {
            wish -= glm::vec3{0.0f, 1.0f, 0.0f};
        }
        if (glm::dot(wish, wish) > 0.0f) {
            camera.position += glm::normalize(wish) * move_speed * dt;
        }
    }
};

}  // namespace game
