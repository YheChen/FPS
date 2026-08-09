#include "game/shared/kill_cam.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using Catch::Approx;

game::ViewSample sample(float x) {
    return game::ViewSample{{x, 0.0f, 0.0f}, x * 0.1f, x * 0.01f};
}

}  // namespace

TEST_CASE("a short trail returns everything it has, oldest first", "[killcam]") {
    game::ViewTrail trail;
    CHECK(trail.size() == 0);
    CHECK(trail.recent(10).empty());

    for (int i = 0; i < 5; ++i) {
        trail.push(sample(static_cast<float>(i)));
    }
    // A player killed two seconds after spawning has a short story, not an
    // invalid one: asking for more than exists returns what exists.
    const std::vector<game::ViewSample> got = trail.recent(40);
    REQUIRE(got.size() == 5);
    CHECK(got.front().position.x == Approx(0.0f));
    CHECK(got.back().position.x == Approx(4.0f));
}

// The ring is the whole point: a trail runs for as long as a life does, and
// must cost the same whether that is two seconds or ten minutes.
TEST_CASE("a full trail keeps the newest samples and stays bounded", "[killcam]") {
    game::ViewTrail trail;
    // Three times capacity, so the write head wraps repeatedly.
    const int total = static_cast<int>(game::kKillCamSamples) * 3;
    for (int i = 0; i < total; ++i) {
        trail.push(sample(static_cast<float>(i)));
    }
    CHECK(trail.size() == game::kKillCamSamples);

    const std::vector<game::ViewSample> got = trail.recent(game::kKillCamSamples);
    REQUIRE(got.size() == game::kKillCamSamples);
    // Oldest first, ending on the most recent push -- the moment of the shot.
    CHECK(got.back().position.x == Approx(static_cast<float>(total - 1)));
    CHECK(got.front().position.x ==
          Approx(static_cast<float>(total - static_cast<int>(game::kKillCamSamples))));
    // Contiguous and in order, which is what playback assumes.
    for (std::size_t i = 1; i < got.size(); ++i) {
        CHECK(got[i].position.x == Approx(got[i - 1].position.x + 1.0f));
    }
}

TEST_CASE("asking for fewer samples returns the most recent ones", "[killcam]") {
    game::ViewTrail trail;
    for (int i = 0; i < 30; ++i) {
        trail.push(sample(static_cast<float>(i)));
    }
    const std::vector<game::ViewSample> got = trail.recent(5);
    REQUIRE(got.size() == 5);
    CHECK(got.front().position.x == Approx(25.0f));
    CHECK(got.back().position.x == Approx(29.0f));
}

// Called on respawn. Without it a later death could replay footage from a
// previous life -- the victim shown a viewpoint from before they existed.
TEST_CASE("clearing a trail forgets the previous life entirely", "[killcam]") {
    game::ViewTrail trail;
    for (int i = 0; i < 20; ++i) {
        trail.push(sample(static_cast<float>(i)));
    }
    trail.clear();
    CHECK(trail.size() == 0);
    CHECK(trail.recent(40).empty());

    trail.push(sample(99.0f));
    const std::vector<game::ViewSample> got = trail.recent(40);
    REQUIRE(got.size() == 1);
    CHECK(got.front().position.x == Approx(99.0f));
}

// Angles ride along with position because a killcam without them shows where
// the killer stood but not what they were looking at, which is the question
// the victim actually has.
TEST_CASE("view angles survive the round trip", "[killcam]") {
    game::ViewTrail trail;
    trail.push(game::ViewSample{{1.0f, 2.0f, 3.0f}, 1.25f, -0.5f});
    const std::vector<game::ViewSample> got = trail.recent(1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].position.y == Approx(2.0f));
    CHECK(got[0].yaw == Approx(1.25f));
    CHECK(got[0].pitch == Approx(-0.5f));
}
