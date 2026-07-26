#pragma once

#include <array>
#include <limits>

#include <glm/glm.hpp>

// Directional-light math. Headless-safe (no GL): this is the part of shadow
// mapping that is worth testing, so it lives in `engine` rather than
// `engine_platform`.
namespace eng {

// Axis-aligned bounding box. `min > max` on any axis means "empty".
struct Bounds {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }

    void expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    // Expands to contain a box that has been placed by `transform`.
    void expand(const Bounds& box, const glm::mat4& transform);

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extent() const { return max - min; }

    std::array<glm::vec3, 8> corners() const;
};

// View-projection matrix a directional light uses to render a shadow map
// that covers `bounds`. The projection is orthographic (a directional light
// has no position) and fitted tightly to the bounds, which is what keeps
// texel density usable: a shadow map stretched over more world than it
// needs to cover is the usual cause of blocky shadows.
//
// `light_direction` points the way the light travels (down, for a sun); it
// need not be normalized. An empty `bounds` yields the identity matrix.
glm::mat4 directional_light_view_projection(const glm::vec3& light_direction, const Bounds& bounds);

}  // namespace eng
