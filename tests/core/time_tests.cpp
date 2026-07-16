#include "engine/core/time.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

constexpr double kTick = 1.0 / 60.0;

TEST_CASE("fixed timestep produces no tick before a full tick accumulates", "[time]") {
    eng::FixedTimestep step{kTick};
    step.advance(kTick * 0.5);
    CHECK_FALSE(step.consume_tick());
    CHECK(step.tick_count() == 0);
}

TEST_CASE("fixed timestep produces exactly the accumulated ticks", "[time]") {
    eng::FixedTimestep step{kTick};
    step.advance(kTick * 3.5);

    int ticks = 0;
    while (step.consume_tick()) {
        ++ticks;
    }
    CHECK(ticks == 3);
    CHECK(step.tick_count() == 3);
    CHECK(step.alpha() == Catch::Approx(0.5).margin(1e-9));
}

TEST_CASE("fixed timestep alpha stays in [0, 1)", "[time]") {
    eng::FixedTimestep step{kTick};
    step.advance(kTick * 0.75);
    CHECK(step.alpha() >= 0.0);
    CHECK(step.alpha() < 1.0);
    CHECK(step.alpha() == Catch::Approx(0.75));
}

TEST_CASE("fixed timestep caps pending ticks after a long stall", "[time]") {
    eng::FixedTimestep step{kTick};
    step.advance(10.0);  // ~600 ticks worth; must be capped

    int ticks = 0;
    while (step.consume_tick()) {
        ++ticks;
    }
    CHECK(ticks == eng::FixedTimestep::kMaxPendingTicks);
}

TEST_CASE("clock is monotonic and clamps huge frames", "[time]") {
    eng::Clock clock;
    const double dt = clock.tick();
    CHECK(dt >= 0.0);
    CHECK(dt <= eng::Clock::kMaxFrameSeconds);
    CHECK(clock.elapsed() >= 0.0);
}

}  // namespace
