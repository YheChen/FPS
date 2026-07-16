#include "game/shared/interpolation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

namespace {

using Catch::Approx;

game::RemoteSample sample_at(std::uint32_t tick, float x, float yaw = 0.0f) {
    game::RemoteSample s;
    s.tick = tick;
    s.position = {x, 0.0f, 0.0f};
    s.yaw = yaw;
    return s;
}

TEST_CASE("buffer interpolates linearly between samples", "[interp]") {
    game::SnapshotBuffer buffer;
    buffer.push(sample_at(10, 0.0f));
    buffer.push(sample_at(13, 3.0f));

    const auto mid = buffer.sample(11.5);
    REQUIRE(mid.has_value());
    CHECK(mid->position.x == Approx(1.5f));

    const auto quarter = buffer.sample(10.75);
    REQUIRE(quarter.has_value());
    CHECK(quarter->position.x == Approx(0.75f));
}

TEST_CASE("buffer clamps outside the sampled range", "[interp]") {
    game::SnapshotBuffer buffer;
    CHECK_FALSE(buffer.sample(5.0).has_value());  // empty

    buffer.push(sample_at(10, 1.0f));
    buffer.push(sample_at(13, 4.0f));
    CHECK(buffer.sample(2.0)->position.x == Approx(1.0f));   // before oldest
    CHECK(buffer.sample(99.0)->position.x == Approx(4.0f));  // after newest
}

TEST_CASE("non-increasing pushes are ignored", "[interp]") {
    game::SnapshotBuffer buffer;
    buffer.push(sample_at(10, 1.0f));
    buffer.push(sample_at(10, 99.0f));  // duplicate tick
    buffer.push(sample_at(8, 55.0f));   // older tick
    CHECK(buffer.size() == 1);
    CHECK(buffer.sample(10.0)->position.x == Approx(1.0f));
}

TEST_CASE("yaw interpolates across the wrap boundary", "[interp]") {
    constexpr float kPi = std::numbers::pi_v<float>;
    game::SnapshotBuffer buffer;
    buffer.push(sample_at(0, 0.0f, kPi - 0.1f));
    buffer.push(sample_at(2, 0.0f, -kPi + 0.1f));  // 0.2 rad away through the wrap

    const auto mid = buffer.sample(1.0);
    REQUIRE(mid.has_value());
    // Shortest arc passes through +-pi, not through zero.
    CHECK(std::abs(std::remainder(mid->yaw - kPi, 2.0f * kPi)) == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("prune keeps a bracketing sample", "[interp]") {
    game::SnapshotBuffer buffer;
    for (std::uint32_t t = 0; t < 10; ++t) {
        buffer.push(sample_at(t * 3, static_cast<float>(t)));
    }
    buffer.prune_before(13);
    // Sample at 13 must still interpolate (needs the sample at tick 12).
    const auto s = buffer.sample(13.0);
    REQUIRE(s.has_value());
    CHECK(s->position.x == Approx(4.0f + 1.0f / 3.0f).margin(1e-4f));
}

}  // namespace
