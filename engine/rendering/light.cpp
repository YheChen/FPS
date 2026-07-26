#include "engine/rendering/light.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace eng {

void Bounds::expand(const Bounds& box, const glm::mat4& transform) {
    if (box.empty()) {
        return;
    }
    for (const glm::vec3& corner : box.corners()) {
        expand(glm::vec3(transform * glm::vec4(corner, 1.0f)));
    }
}

std::array<glm::vec3, 8> Bounds::corners() const {
    return {
        glm::vec3{min.x, min.y, min.z}, glm::vec3{max.x, min.y, min.z},
        glm::vec3{min.x, max.y, min.z}, glm::vec3{max.x, max.y, min.z},
        glm::vec3{min.x, min.y, max.z}, glm::vec3{max.x, min.y, max.z},
        glm::vec3{min.x, max.y, max.z}, glm::vec3{max.x, max.y, max.z},
    };
}

glm::mat4 directional_light_view_projection(const glm::vec3& light_direction,
                                            const Bounds& bounds) {
    if (bounds.empty()) {
        return glm::mat4{1.0f};
    }

    const float length = glm::length(light_direction);
    if (length < 1e-6f) {
        return glm::mat4{1.0f};
    }
    const glm::vec3 direction = light_direction / length;

    // lookAt degenerates when `up` is parallel to the view direction, which
    // is exactly the common case here (a sun pointing straight down).
    const glm::vec3 up =
        std::abs(direction.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};

    const glm::vec3 center = bounds.center();
    const glm::mat4 view = glm::lookAt(center, center + direction, up);

    // Fit the projection to the bounds as seen from the light. Anything
    // looser wastes shadow-map resolution.
    glm::vec3 light_min{std::numeric_limits<float>::max()};
    glm::vec3 light_max{std::numeric_limits<float>::lowest()};
    for (const glm::vec3& corner : bounds.corners()) {
        const glm::vec3 in_light_space = glm::vec3(view * glm::vec4(corner, 1.0f));
        light_min = glm::min(light_min, in_light_space);
        light_max = glm::max(light_max, in_light_space);
    }

    // The view looks down -Z, so the near plane is at -light_max.z. The eye
    // sits at the center of the bounds, which puts half the box behind it;
    // a negative near distance is fine for an orthographic projection (the
    // mapping is affine) and avoids clipping casters on the light's side.
    return glm::ortho(light_min.x, light_max.x, light_min.y, light_max.y, -light_max.z,
                      -light_min.z) *
           view;
}

}  // namespace eng
