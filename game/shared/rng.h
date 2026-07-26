#pragma once

#include <cstdint>

#include <glm/glm.hpp>

// Deterministic, stateless randomness for gameplay.
//
// Weapon spread must be reproducible: the server is the only authority that
// rolls it, but a recorded match has to replay bit-for-bit (Milestone 17), so
// we never use std::rand or a wall-clock seed. Instead every roll is a pure
// hash of (tick, player, shot index) - same inputs, same pellets, forever.
namespace game {

// Integer finalizer from MurmurHash3. Cheap, good avalanche.
constexpr std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

constexpr std::uint32_t hash_combine(std::uint32_t a, std::uint32_t b) {
    return hash_u32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2)));
}

// Uniform float in [0, 1) from a seed.
constexpr float hash_float01(std::uint32_t seed) {
    // Top 24 bits give an exactly representable float mantissa.
    return static_cast<float>(hash_u32(seed) >> 8) * (1.0f / 16777216.0f);
}

// Builds an orthonormal basis around `forward` (assumed normalized).
inline void basis_from_forward(const glm::vec3& forward, glm::vec3& right, glm::vec3& up) {
    // Pick the world axis least aligned with forward to avoid degeneracy.
    const glm::vec3 reference =
        (std::abs(forward.y) < 0.99f) ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    right = glm::normalize(glm::cross(forward, reference));
    up = glm::cross(right, forward);
}

// Deterministically perturbs `forward` within a cone of `spread_radians`.
// Uses a concentric-disc mapping so pellets are uniform across the cone face
// rather than clustered at the center.
inline glm::vec3 spread_direction(const glm::vec3& forward, float spread_radians,
                                  std::uint32_t seed) {
    if (spread_radians <= 0.0f) {
        return forward;
    }
    const float u0 = hash_float01(seed);
    const float u1 = hash_float01(seed ^ 0x5bf03635u);
    const float radius = spread_radians * std::sqrt(u0);
    const float angle = u1 * 6.28318530718f;

    glm::vec3 right;
    glm::vec3 up;
    basis_from_forward(forward, right, up);
    const glm::vec3 offset = right * (std::cos(angle) * radius) + up * (std::sin(angle) * radius);
    return glm::normalize(forward + offset);
}

}  // namespace game
