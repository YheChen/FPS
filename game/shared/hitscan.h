#pragma once

#include <cmath>
#include <optional>

#include <glm/glm.hpp>

// Analytic intersection helpers for hitscan weapons. Pure math, unit-tested;
// used against player/target hit volumes (the world itself uses Jolt rays).
namespace game {

// Distance along the (normalized) ray to the closest sphere intersection,
// nullopt on miss or if the sphere is entirely behind the origin. An origin
// inside the sphere hits at distance 0.
inline std::optional<float> ray_sphere(const glm::vec3& origin, const glm::vec3& direction,
                                       const glm::vec3& center, float radius) {
    const glm::vec3 oc = origin - center;
    const float b = glm::dot(oc, direction);
    const float c = glm::dot(oc, oc) - radius * radius;
    if (c <= 0.0f) {
        return 0.0f;  // inside
    }
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    const float t = -b - std::sqrt(discriminant);
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

// Vertical capsule from `feet` with the given total height and radius
// (matches CharacterController). Analytic: infinite-cylinder intersection
// clipped to the cylinder segment, plus the two cap spheres.
inline std::optional<float> ray_vertical_capsule(const glm::vec3& origin,
                                                 const glm::vec3& direction,
                                                 const glm::vec3& feet, float radius,
                                                 float height) {
    const float y_bottom = feet.y + radius;          // bottom sphere center height
    const float y_top = feet.y + height - radius;    // top sphere center height
    std::optional<float> best;
    const auto consider = [&best](std::optional<float> t) {
        if (t && (!best || *t < *best)) {
            best = t;
        }
    };

    // Infinite cylinder around the vertical axis through (feet.x, feet.z).
    const float ox = origin.x - feet.x;
    const float oz = origin.z - feet.z;
    const float a = direction.x * direction.x + direction.z * direction.z;
    const float b = ox * direction.x + oz * direction.z;
    const float c = ox * ox + oz * oz - radius * radius;
    if (a > 1e-8f) {
        const float discriminant = b * b - a * c;
        if (discriminant >= 0.0f) {
            const float t = (-b - std::sqrt(discriminant)) / a;
            if (t >= 0.0f) {
                const float y = origin.y + direction.y * t;
                if (y >= y_bottom && y <= y_top) {
                    consider(t);
                }
            }
        }
    } else if (c <= 0.0f && origin.y >= y_bottom && origin.y <= y_top) {
        consider(0.0f);  // vertical ray already inside the cylinder wall
    }

    // Cap spheres.
    consider(ray_sphere(origin, direction, {feet.x, y_bottom, feet.z}, radius));
    consider(ray_sphere(origin, direction, {feet.x, y_top, feet.z}, radius));
    return best;
}

}  // namespace game
