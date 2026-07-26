#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/particle_sim.h"
#include "engine/rendering/shader.h"

namespace eng {

// Draws a ParticlePool as camera-facing quads in a single instanced draw
// call. Move-only, RAII.
//
// Blending is premultiplied alpha (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), which
// gets both looks from one pass: alpha 0 with bright RGB reads as additive
// glow (sparks, muzzle flash), alpha 1 as an opaque puff (smoke, blood).
//
// Particles test depth but do not write it, so they are correctly occluded
// by world geometry without occluding each other.
class ParticleRenderer {
public:
    static std::optional<ParticleRenderer> create(std::size_t max_particles);

    ~ParticleRenderer();
    ParticleRenderer(ParticleRenderer&& other) noexcept;
    ParticleRenderer& operator=(ParticleRenderer&& other) noexcept;
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // `camera_right`/`camera_up` orient the quads; pass the camera basis so
    // every particle faces the viewer.
    void draw(const ParticlePool& pool, const glm::mat4& view_projection,
              const glm::vec3& camera_right, const glm::vec3& camera_up);

    // Instances submitted by the last draw() call.
    std::size_t last_instance_count() const { return last_instance_count_; }

private:
    ParticleRenderer() = default;

    // Per-instance GPU layout; must match the vertex attributes set up in
    // create().
    struct Instance {
        glm::vec3 position;
        float size;
        glm::vec4 color;
    };

    std::uint32_t vao_ = 0;
    std::uint32_t quad_vbo_ = 0;
    std::uint32_t instance_vbo_ = 0;
    std::optional<Shader> shader_;
    std::size_t max_particles_ = 0;
    std::size_t last_instance_count_ = 0;
    std::vector<Instance> instances_;
};

}  // namespace eng
