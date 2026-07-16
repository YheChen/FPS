#include "game/shared/lag_comp.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("position history returns exact recorded positions", "[lagcomp]") {
    game::PositionHistory history;
    for (std::uint32_t tick = 100; tick <= 130; ++tick) {
        history.push(tick, {static_cast<float>(tick), 0.0f, 0.0f});
    }
    CHECK(history.latest_tick() == 130);

    const auto at_120 = history.at(120);
    REQUIRE(at_120.has_value());
    CHECK(at_120->x == Catch::Approx(120.0f));

    // Overwritten by the ring (capacity 32): tick 90 was never recorded,
    // and tick 98 has been evicted by tick 130.
    CHECK_FALSE(history.at(90).has_value());
    CHECK_FALSE(history.at(98).has_value());
}

TEST_CASE("history slots verify the tick, not just the ring index", "[lagcomp]") {
    game::PositionHistory history;
    history.push(5, {1.0f, 0.0f, 0.0f});
    // Same ring slot (5 + 32), different tick: must not alias.
    CHECK_FALSE(history.at(37).has_value());
    history.push(37, {2.0f, 0.0f, 0.0f});
    CHECK(history.at(37)->x == Catch::Approx(2.0f));
    CHECK_FALSE(history.at(5).has_value());  // evicted
}

TEST_CASE("rewind tick claims are clamped to the legal window", "[lagcomp]") {
    const std::uint32_t now = 1000;

    // Honest claims inside the window pass through.
    CHECK(game::clamp_rewind_tick(995, now) == 995);
    CHECK(game::clamp_rewind_tick(now - game::kMaxRewindTicks, now) == now - game::kMaxRewindTicks);

    // Hostile claims: ancient past clamps to the window edge; the future
    // and "no estimate" resolve to now.
    CHECK(game::clamp_rewind_tick(1, now) == now - game::kMaxRewindTicks);
    CHECK(game::clamp_rewind_tick(now + 50, now) == now);
    CHECK(game::clamp_rewind_tick(0, now) == now);

    // Early-match edge: window floor never underflows.
    CHECK(game::clamp_rewind_tick(2, 5) == 2);
    CHECK(game::clamp_rewind_tick(0, 5) == 5);
}

}  // namespace
