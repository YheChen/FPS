#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

// Analytic intersection helpers for hitscan weapons. Pure math, unit-tested;
// used against player/target hit volumes (the world itself uses Jolt rays).
namespace game {

// View direction from yaw/pitch (camera convention: yaw=0 faces -Z,
// positive pitch looks up). Matches eng::Camera::forward.
inline glm::vec3 view_direction(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    return glm::normalize(glm::vec3{std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp});
}

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
                                                 const glm::vec3& direction, const glm::vec3& feet,
                                                 float radius, float height) {
    const float y_bottom = feet.y + radius;        // bottom sphere center height
    const float y_top = feet.y + height - radius;  // top sphere center height
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

// Where on a player a shot landed. Players are one capsule, not a rig of
// per-bone volumes, so a zone is derived from WHERE ON THAT CAPSULE the ray
// struck rather than from an animated skeleton. That is deliberate: lag
// compensation (game/shared/lag_comp.h) rewinds POSITIONS only, so anything
// needing a historical pose could not be rewound and would resolve hits
// against a pose the shooter never saw.
enum class HitZone : std::uint8_t {
    Torso = 0,
    Head = 1,
    Arm = 2,
    Leg = 3,
};

// Proportions of a standing figure as fractions of total capsule height, so
// the same numbers apply to a crouched player (whose capsule is shorter)
// without a second set of constants.
struct HitZoneShape {
    // At or above this fraction of height is the head. 0.86 of a 1.8 m
    // capsule is ~1.55 m: the neck.
    float head_fraction = 0.86f;
    // At or below this is legs. 0.47 is roughly the hip.
    float legs_fraction = 0.47f;
    // Within the torso band, a hit this far out from the capsule's vertical
    // axis (as a fraction of radius) caught an arm rather than the body.
    float arm_radius_fraction = 0.55f;
};

inline constexpr HitZoneShape kHitZoneShape{};

// How far a ray passes from a player's vertical axis, measured horizontally
// and perpendicular to the shot: 0 straight through the middle, ~radius for
// a graze along the silhouette's edge.
//
// This is deliberately a property of the RAY, not of the impact point. A ray
// striking a cylinder always lands on its surface, so the impact point is
// always exactly `radius` from the axis -- measuring there would classify
// every single hit as an edge graze.
inline float ray_lateral_offset(const glm::vec3& origin, const glm::vec3& direction,
                                const glm::vec3& feet) {
    glm::vec2 d{direction.x, direction.z};
    const float length = glm::length(d);
    if (length < 1e-6f) {
        return 0.0f;  // shot straight up or down: no lateral offset to speak of
    }
    d /= length;
    const glm::vec2 to_axis{feet.x - origin.x, feet.z - origin.z};
    // 2D cross product: the perpendicular distance from the axis to the line.
    return std::abs(to_axis.x * d.y - to_axis.y * d.x);
}

// Classifies where a shot landed on a player's capsule. `hit_point` gives the
// height, and the ray gives how centrally it passed. `feet` is the capsule
// base and `height` its total height, matching ray_vertical_capsule.
//
// Pure and deterministic: the server resolves damage with it, and a replay of
// the same shot must classify the same way.
inline HitZone classify_hit_zone(const glm::vec3& hit_point, const glm::vec3& ray_origin,
                                 const glm::vec3& ray_direction, const glm::vec3& feet,
                                 float radius, float height,
                                 const HitZoneShape& shape = kHitZoneShape) {
    if (!(height > 0.0f)) {
        return HitZone::Torso;  // degenerate capsule: no zone is meaningful
    }
    const float fraction = std::clamp((hit_point.y - feet.y) / height, 0.0f, 1.0f);
    if (fraction >= shape.head_fraction) {
        return HitZone::Head;
    }
    if (fraction <= shape.legs_fraction) {
        return HitZone::Leg;
    }
    // Torso band: a shot down the middle is body, one out by the silhouette's
    // edge caught an arm.
    if (radius > 1e-6f) {
        const float lateral = ray_lateral_offset(ray_origin, ray_direction, feet);
        if (lateral >= shape.arm_radius_fraction * radius) {
            return HitZone::Arm;
        }
    }
    return HitZone::Torso;
}

}  // namespace game
