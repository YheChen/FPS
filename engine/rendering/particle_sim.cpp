#include "engine/rendering/particle_sim.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eng {

namespace {

// A local hash, deliberately not game/shared/rng.h: gameplay randomness has
// to stay bit-exact across client and server, and nothing cosmetic should
// be able to reach into it.
std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash_unit(std::uint32_t seed) {
    // 24 bits of mantissa is plenty and avoids ever returning exactly 1.0.
    return static_cast<float>(hash_u32(seed) >> 8) / static_cast<float>(1u << 24);
}

// Orthonormal basis around `forward`, avoiding the degenerate cross product
// when forward is parallel to the axis used to build it.
void basis_from(const glm::vec3& forward, glm::vec3& right, glm::vec3& up) {
    const glm::vec3 reference =
        std::abs(forward.y) > 0.99f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
    right = glm::normalize(glm::cross(forward, reference));
    up = glm::cross(right, forward);
}

}  // namespace

ParticlePool::ParticlePool(std::size_t capacity) : capacity_(capacity) {
    particles_.reserve(capacity);
}

std::size_t ParticlePool::emit(const EmitParams& params, std::uint32_t seed) {
    if (params.count <= 0 || capacity_ == 0) {
        return 0;
    }

    const float direction_length = glm::length(params.direction);
    const glm::vec3 forward = direction_length > 1e-6f ? params.direction / direction_length
                                                       : glm::vec3{0.0f, 1.0f, 0.0f};
    glm::vec3 right{};
    glm::vec3 up{};
    basis_from(forward, right, up);

    const auto wanted = static_cast<std::size_t>(params.count);
    const std::size_t room = capacity_ - std::min(capacity_, particles_.size());
    const std::size_t emitted = std::min(wanted, room);

    for (std::size_t i = 0; i < emitted; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const float u_angle = hash_unit(hash_u32(seed ^ 0x9e3779b9u) + index * 3u);
        const float u_radius = hash_unit(hash_u32(seed ^ 0x85ebca6bu) + index * 5u);
        const float u_speed = hash_unit(hash_u32(seed ^ 0xc2b2ae35u) + index * 7u);
        const float u_life = hash_unit(hash_u32(seed ^ 0x27d4eb2fu) + index * 11u);

        // sqrt keeps the sample density even across the cone's disc rather
        // than bunching it at the centre.
        const float angle = u_angle * 2.0f * std::numbers::pi_v<float>;
        const float radius = std::sqrt(u_radius) * std::tan(params.cone_radians);
        const glm::vec3 direction = glm::normalize(forward + right * (std::cos(angle) * radius) +
                                                   up * (std::sin(angle) * radius));

        const float speed = params.speed * (1.0f + params.speed_jitter * (u_speed * 2.0f - 1.0f));
        const float lifetime =
            std::max(1e-3f, params.lifetime_seconds *
                                (1.0f + params.lifetime_jitter * (u_life * 2.0f - 1.0f)));

        Particle particle;
        particle.position = params.position;
        particle.velocity = direction * speed;
        particle.color_start = params.color_start;
        particle.color_end = params.color_end;
        particle.size_start = params.size_start;
        particle.size_end = params.size_end;
        particle.lifetime_seconds = lifetime;
        particle.gravity = params.gravity;
        particle.drag = params.drag;
        particles_.push_back(particle);
    }
    return emitted;
}

void ParticlePool::update(float dt_seconds) {
    if (dt_seconds <= 0.0f) {
        return;
    }
    for (Particle& particle : particles_) {
        particle.age_seconds += dt_seconds;
        particle.velocity.y -= particle.gravity * dt_seconds;
        if (particle.drag > 0.0f) {
            // Exponential decay, so the result does not depend on the frame
            // rate the way a linear (1 - drag*dt) damping would.
            particle.velocity *= std::exp(-particle.drag * dt_seconds);
        }
        particle.position += particle.velocity * dt_seconds;
    }
    std::erase_if(particles_, [](const Particle& particle) { return particle.dead(); });
}

glm::vec4 ParticlePool::color_at(const Particle& particle) {
    return glm::mix(particle.color_start, particle.color_end,
                    std::clamp(particle.life_fraction(), 0.0f, 1.0f));
}

float ParticlePool::size_at(const Particle& particle) {
    return glm::mix(particle.size_start, particle.size_end,
                    std::clamp(particle.life_fraction(), 0.0f, 1.0f));
}

}  // namespace eng
