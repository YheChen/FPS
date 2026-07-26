#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// Particle simulation. Headless (no GL) so the integration and lifetime
// rules can be unit-tested; ParticleRenderer draws the result.
//
// Simulated on the CPU, drawn with one instanced draw call. Not a
// compute-shader system: the project targets OpenGL 4.1 / WebGL 2, neither
// of which has compute (see ADR 0003). A few thousand particles per frame
// is well within CPU budget at 60 Hz.
namespace eng {

struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    // Colors are premultiplied-alpha RGBA. An alpha of 0 with non-zero RGB
    // is a purely additive glow, which is how sparks and muzzle flash are
    // expressed without a second blend mode or a second draw call.
    glm::vec4 color_start{1.0f};
    glm::vec4 color_end{0.0f};
    float size_start = 0.1f;  // metres, world-space quad side
    float size_end = 0.0f;
    float age_seconds = 0.0f;
    float lifetime_seconds = 1.0f;
    float gravity = 0.0f;  // m/s^2 applied downward (-Y)
    float drag = 0.0f;     // velocity decay per second, 0 = none

    float life_fraction() const {
        return lifetime_seconds > 0.0f ? age_seconds / lifetime_seconds : 1.0f;
    }
    bool dead() const { return age_seconds >= lifetime_seconds; }
};

// One burst. Directions are sampled in a cone around `direction`.
struct EmitParams {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    float cone_radians = 0.6f;
    float speed = 3.0f;
    float speed_jitter = 0.4f;  // fraction of `speed`, +-
    glm::vec4 color_start{1.0f};
    glm::vec4 color_end{0.0f};
    float size_start = 0.08f;
    float size_end = 0.0f;
    float lifetime_seconds = 0.5f;
    float lifetime_jitter = 0.3f;  // fraction, +-
    float gravity = 0.0f;
    float drag = 2.0f;
    int count = 12;
};

// Fixed-capacity pool. Purely cosmetic: it deliberately shares nothing with
// game/shared/rng.h, because gameplay randomness has to stay bit-exact for
// prediction and replay, and visual effects must never be able to perturb
// it.
class ParticlePool {
public:
    explicit ParticlePool(std::size_t capacity);

    // Emits up to `params.count` particles and returns how many were
    // actually created. A full pool drops the excess rather than growing:
    // an effect firing every frame must not be able to consume memory
    // without bound. `seed` selects the burst's random directions.
    std::size_t emit(const EmitParams& params, std::uint32_t seed);

    void update(float dt_seconds);
    void clear() { particles_.clear(); }

    std::size_t alive() const { return particles_.size(); }
    std::size_t capacity() const { return capacity_; }
    const std::vector<Particle>& particles() const { return particles_; }

    // Appearance interpolated over the particle's life.
    static glm::vec4 color_at(const Particle& particle);
    static float size_at(const Particle& particle);

private:
    std::size_t capacity_;
    std::vector<Particle> particles_;
};

}  // namespace eng
