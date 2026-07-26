#include "engine/rendering/particle_renderer.h"

#include "engine/rendering/gl.h"

#include <array>
#include <string_view>
#include <utility>

#include "engine/core/log.h"

namespace eng {

namespace {

// Each instance is expanded into a camera-facing quad in the vertex shader,
// so the only per-instance data on the bus is centre, size and color.
constexpr std::string_view kVertexSource = R"(
layout(location = 0) in vec2 a_corner;        // unit quad, -0.5 .. 0.5
layout(location = 1) in vec3 a_center;        // per instance
layout(location = 2) in float a_size;         // per instance
layout(location = 3) in vec4 a_color;         // per instance, premultiplied
uniform mat4 u_view_projection;
uniform vec3 u_camera_right;
uniform vec3 u_camera_up;
out vec2 v_corner;
out vec4 v_color;
void main() {
    vec3 offset = (u_camera_right * a_corner.x + u_camera_up * a_corner.y) * a_size;
    v_corner = a_corner;
    v_color = a_color;
    gl_Position = u_view_projection * vec4(a_center + offset, 1.0);
}
)";

// A soft radial falloff, computed rather than sampled: a texture for this
// would be one more asset to ship and load for no visual gain.
constexpr std::string_view kFragmentSource = R"(
in vec2 v_corner;
in vec4 v_color;
out vec4 o_color;
void main() {
    float distance_from_center = length(v_corner) * 2.0;
    float falloff = 1.0 - smoothstep(0.0, 1.0, distance_from_center);
    if (falloff <= 0.0) {
        discard;
    }
    // Color is already premultiplied, so the falloff scales the whole
    // vec4 and the blend stays correct for both additive and opaque
    // particles.
    o_color = v_color * falloff;
}
)";

constexpr std::array<float, 12> kQuadCorners = {
    -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,
};

}  // namespace

std::optional<ParticleRenderer> ParticleRenderer::create(std::size_t max_particles) {
    if (max_particles == 0) {
        log::error("ParticleRenderer: capacity must be positive");
        return std::nullopt;
    }

    ParticleRenderer renderer;
    renderer.max_particles_ = max_particles;
    renderer.instances_.reserve(max_particles);

    renderer.shader_ = Shader::create("particles", kVertexSource, kFragmentSource);
    if (!renderer.shader_) {
        return std::nullopt;
    }

    glGenVertexArrays(1, &renderer.vao_);
    glBindVertexArray(renderer.vao_);

    glGenBuffers(1, &renderer.quad_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, renderer.quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kQuadCorners)),
                 kQuadCorners.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);

    glGenBuffers(1, &renderer.instance_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, renderer.instance_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(max_particles * sizeof(Instance)),
                 nullptr, GL_STREAM_DRAW);

    constexpr auto stride = static_cast<GLsizei>(sizeof(Instance));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Instance, position)));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Instance, size)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Instance, color)));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    log::info("Particle renderer: capacity {}", max_particles);
    return renderer;
}

ParticleRenderer::~ParticleRenderer() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
    }
    if (quad_vbo_ != 0) {
        glDeleteBuffers(1, &quad_vbo_);
    }
    if (instance_vbo_ != 0) {
        glDeleteBuffers(1, &instance_vbo_);
    }
}

ParticleRenderer::ParticleRenderer(ParticleRenderer&& other) noexcept
    : vao_(std::exchange(other.vao_, 0u)),
      quad_vbo_(std::exchange(other.quad_vbo_, 0u)),
      instance_vbo_(std::exchange(other.instance_vbo_, 0u)),
      shader_(std::move(other.shader_)),
      max_particles_(other.max_particles_),
      last_instance_count_(other.last_instance_count_),
      instances_(std::move(other.instances_)) {}

ParticleRenderer& ParticleRenderer::operator=(ParticleRenderer&& other) noexcept {
    if (this != &other) {
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
        }
        if (quad_vbo_ != 0) {
            glDeleteBuffers(1, &quad_vbo_);
        }
        if (instance_vbo_ != 0) {
            glDeleteBuffers(1, &instance_vbo_);
        }
        vao_ = std::exchange(other.vao_, 0u);
        quad_vbo_ = std::exchange(other.quad_vbo_, 0u);
        instance_vbo_ = std::exchange(other.instance_vbo_, 0u);
        shader_ = std::move(other.shader_);
        max_particles_ = other.max_particles_;
        last_instance_count_ = other.last_instance_count_;
        instances_ = std::move(other.instances_);
    }
    return *this;
}

void ParticleRenderer::draw(const ParticlePool& pool, const glm::mat4& view_projection,
                            const glm::vec3& camera_right, const glm::vec3& camera_up) {
    last_instance_count_ = 0;
    if (!shader_ || pool.alive() == 0) {
        return;
    }

    instances_.clear();
    for (const Particle& particle : pool.particles()) {
        if (instances_.size() >= max_particles_) {
            break;
        }
        instances_.push_back(
            {particle.position, ParticlePool::size_at(particle), ParticlePool::color_at(particle)});
    }
    if (instances_.empty()) {
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, instance_vbo_);
    // Orphan the buffer first so the driver can hand back fresh storage
    // instead of stalling until last frame's draw has finished reading it.
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(max_particles_ * sizeof(Instance)),
                 nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(instances_.size() * sizeof(Instance)),
                    instances_.data());

    shader_->bind();
    shader_->set_mat4("u_view_projection", view_projection);
    shader_->set_vec3("u_camera_right", camera_right);
    shader_->set_vec3("u_camera_up", camera_up);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied alpha
    // Occluded by the world, but never by each other: writing depth would
    // make particles of the same burst punch holes in one another.
    glDepthMask(GL_FALSE);

    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(instances_.size()));
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    last_instance_count_ = instances_.size();
}

}  // namespace eng
