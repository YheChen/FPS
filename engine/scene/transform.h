#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace eng {

// Translation * Rotation * Scale, in that order.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 to_matrix() const;

    // Decomposes an affine TRS matrix (no shear/projection expected; shear
    // is silently folded into scale/rotation best-effort).
    static Transform from_matrix(const glm::mat4& matrix);
};

}  // namespace eng
