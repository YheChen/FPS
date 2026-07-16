#include "engine/rendering/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eng {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
constexpr float kPi = std::numbers::pi_v<float>;
}  // namespace

glm::vec3 Camera::forward() const {
    const float cp = std::cos(pitch);
    return glm::normalize(glm::vec3{std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp});
}

glm::vec3 Camera::right() const {
    return glm::normalize(glm::cross(forward(), kWorldUp));
}

void Camera::clamp_angles() {
    pitch = std::clamp(pitch, -kMaxPitchRadians, kMaxPitchRadians);
    yaw = std::remainder(yaw, 2.0f * kPi);
}

glm::mat4 Camera::view() const {
    return glm::lookAt(position, position + forward(), kWorldUp);
}

glm::mat4 Camera::projection() const {
    return glm::perspective(glm::radians(fov_y_degrees), aspect, near_plane, far_plane);
}

}  // namespace eng
