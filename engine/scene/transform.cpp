#include "engine/scene/transform.h"

#include <glm/gtc/matrix_transform.hpp>

namespace eng {

glm::mat4 Transform::to_matrix() const {
    glm::mat4 m = glm::translate(glm::mat4{1.0f}, position);
    m = m * glm::mat4_cast(rotation);
    m = glm::scale(m, scale);
    return m;
}

Transform Transform::from_matrix(const glm::mat4& matrix) {
    Transform t;
    t.position = glm::vec3(matrix[3]);

    glm::vec3 col0 = glm::vec3(matrix[0]);
    glm::vec3 col1 = glm::vec3(matrix[1]);
    glm::vec3 col2 = glm::vec3(matrix[2]);
    t.scale = {glm::length(col0), glm::length(col1), glm::length(col2)};

    // Guard degenerate scale before normalizing.
    if (t.scale.x > 0.0f) {
        col0 /= t.scale.x;
    }
    if (t.scale.y > 0.0f) {
        col1 /= t.scale.y;
    }
    if (t.scale.z > 0.0f) {
        col2 /= t.scale.z;
    }
    // Negative determinant means a mirrored basis; fold the flip into X.
    if (glm::dot(glm::cross(col0, col1), col2) < 0.0f) {
        t.scale.x = -t.scale.x;
        col0 = -col0;
    }
    t.rotation = glm::quat_cast(glm::mat3{col0, col1, col2});
    return t;
}

}  // namespace eng
