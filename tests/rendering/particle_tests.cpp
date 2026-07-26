#include "engine/rendering/particle_sim.h"

#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using Catch::Approx;

eng::EmitParams basic_burst(int count = 10) {
    eng::EmitParams params;
    params.position = {1.0f, 2.0f, 3.0f};
    params.direction = {0.0f, 1.0f, 0.0f};
    params.cone_radians = 0.4f;
    params.speed = 5.0f;
    params.speed_jitter = 0.0f;
    params.lifetime_seconds = 1.0f;
    params.lifetime_jitter = 0.0f;
    params.gravity = 0.0f;
    params.drag = 0.0f;
    params.count = count;
    return params;
}

TEST_CASE("emit fills the pool and then drops the excess", "[particles]") {
    eng::ParticlePool pool{16};
    CHECK(pool.capacity() == 16);
    CHECK(pool.alive() == 0);

    CHECK(pool.emit(basic_burst(10), 1u) == 10);
    CHECK(pool.alive() == 10);

    // Only 6 slots left: the burst is clipped, not the pool grown.
    CHECK(pool.emit(basic_burst(10), 2u) == 6);
    CHECK(pool.alive() == 16);

    // Full pool: nothing more, and no crash.
    CHECK(pool.emit(basic_burst(10), 3u) == 0);
    CHECK(pool.alive() == 16);
}

TEST_CASE("emitted particles start at the emitter and inside the cone", "[particles]") {
    eng::ParticlePool pool{256};
    eng::EmitParams params = basic_burst(200);
    pool.emit(params, 7u);
    REQUIRE(pool.alive() == 200);

    const glm::vec3 forward = glm::normalize(params.direction);
    std::size_t distinct_directions = 0;
    glm::vec3 previous{0.0f};
    for (const eng::Particle& particle : pool.particles()) {
        CHECK(particle.position.x == Approx(params.position.x));
        CHECK(particle.position.y == Approx(params.position.y));
        CHECK(particle.position.z == Approx(params.position.z));

        // speed_jitter is 0, so every particle travels at exactly `speed`.
        CHECK(glm::length(particle.velocity) == Approx(params.speed).margin(1e-4f));

        const glm::vec3 direction = glm::normalize(particle.velocity);
        const float angle = std::acos(std::clamp(glm::dot(direction, forward), -1.0f, 1.0f));
        CHECK(angle <= params.cone_radians + 1e-3f);

        if (glm::length(direction - previous) > 1e-6f) {
            ++distinct_directions;
        }
        previous = direction;
    }
    CHECK(distinct_directions > 150);  // genuinely scattered, not a constant
}

TEST_CASE("particles die exactly when their lifetime runs out", "[particles]") {
    eng::ParticlePool pool{64};
    eng::EmitParams params = basic_burst(5);
    params.lifetime_seconds = 0.5f;
    pool.emit(params, 1u);
    REQUIRE(pool.alive() == 5);

    for (int i = 0; i < 29; ++i) {
        pool.update(1.0f / 60.0f);
    }
    CHECK(pool.alive() == 5);  // 0.483 s, still inside the lifetime

    pool.update(1.0f / 60.0f);
    pool.update(1.0f / 60.0f);
    CHECK(pool.alive() == 0);  // past 0.5 s
}

TEST_CASE("gravity and drag integrate as expected", "[particles]") {
    eng::ParticlePool gravity_pool{4};
    eng::EmitParams params = basic_burst(1);
    params.direction = {0.0f, 1.0f, 0.0f};
    params.cone_radians = 0.0f;
    params.speed = 0.0f;
    params.gravity = 10.0f;
    params.lifetime_seconds = 10.0f;
    gravity_pool.emit(params, 1u);

    gravity_pool.update(1.0f);
    const eng::Particle& falling = gravity_pool.particles()[0];
    CHECK(falling.velocity.y == Approx(-10.0f));
    CHECK(falling.position.y == Approx(params.position.y - 10.0f));

    // Drag is exponential, so it must not depend on how the interval is
    // split -- a linear (1 - drag*dt) damping would fail this.
    eng::EmitParams dragged = basic_burst(1);
    dragged.cone_radians = 0.0f;
    dragged.speed = 8.0f;
    dragged.drag = 3.0f;
    dragged.lifetime_seconds = 10.0f;

    eng::ParticlePool one_step{4};
    one_step.emit(dragged, 1u);
    one_step.update(0.5f);

    eng::ParticlePool many_steps{4};
    many_steps.emit(dragged, 1u);
    for (int i = 0; i < 50; ++i) {
        many_steps.update(0.01f);
    }

    CHECK(glm::length(one_step.particles()[0].velocity) ==
          Approx(glm::length(many_steps.particles()[0].velocity)).margin(1e-3f));
    CHECK(glm::length(one_step.particles()[0].velocity) ==
          Approx(8.0f * std::exp(-1.5f)).margin(1e-3f));
}

TEST_CASE("appearance interpolates from start to end over the lifetime", "[particles]") {
    eng::ParticlePool pool{4};
    eng::EmitParams params = basic_burst(1);
    params.speed = 0.0f;
    params.lifetime_seconds = 1.0f;
    params.color_start = {1.0f, 0.5f, 0.0f, 1.0f};
    params.color_end = {0.0f, 0.0f, 0.0f, 0.0f};
    params.size_start = 1.0f;
    params.size_end = 0.0f;
    pool.emit(params, 1u);

    const eng::Particle fresh = pool.particles()[0];
    CHECK(eng::ParticlePool::size_at(fresh) == Approx(1.0f));
    CHECK(eng::ParticlePool::color_at(fresh).r == Approx(1.0f));
    CHECK(fresh.life_fraction() == Approx(0.0f));

    pool.update(0.5f);
    const eng::Particle half = pool.particles()[0];
    CHECK(half.life_fraction() == Approx(0.5f));
    CHECK(eng::ParticlePool::size_at(half) == Approx(0.5f));
    CHECK(eng::ParticlePool::color_at(half).r == Approx(0.5f));
    CHECK(eng::ParticlePool::color_at(half).a == Approx(0.5f));
}

TEST_CASE("degenerate emissions are ignored rather than crashing", "[particles]") {
    eng::ParticlePool pool{16};

    eng::EmitParams none = basic_burst(0);
    CHECK(pool.emit(none, 1u) == 0);

    eng::EmitParams negative = basic_burst(-5);
    CHECK(pool.emit(negative, 1u) == 0);

    // A zero direction must not produce NaN velocities.
    eng::EmitParams no_direction = basic_burst(4);
    no_direction.direction = {0.0f, 0.0f, 0.0f};
    CHECK(no_direction.count == 4);
    CHECK(pool.emit(no_direction, 1u) == 4);
    for (const eng::Particle& particle : pool.particles()) {
        CHECK(std::isfinite(particle.velocity.x));
        CHECK(std::isfinite(particle.velocity.y));
        CHECK(std::isfinite(particle.velocity.z));
    }

    // A non-positive step is a no-op, not a backwards integration.
    const glm::vec3 before = pool.particles()[0].position;
    pool.update(0.0f);
    pool.update(-1.0f);
    CHECK(pool.particles()[0].position.y == Approx(before.y));
    CHECK(pool.particles()[0].age_seconds == Approx(0.0f));
}

TEST_CASE("a zero-capacity pool accepts nothing", "[particles]") {
    eng::ParticlePool pool{0};
    CHECK(pool.emit(basic_burst(10), 1u) == 0);
    CHECK(pool.alive() == 0);
    pool.update(0.016f);  // must not crash
}

}  // namespace
