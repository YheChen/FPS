#include "game/shared/footsteps.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "game/shared/player_movement.h"

namespace {

using Catch::Approx;

// Walks a figure `distance` metres in a straight line on the ground and
// returns every step it fired. `dt` only decides how finely the ground is
// chopped up: the module never reads a clock, which is the property most of
// these tests exist to pin down.
std::vector<game::FootstepEvent> walk(game::FootstepState& state, float speed, float distance,
                                      float dt = 1.0f / 60.0f, bool crouching = false) {
    std::vector<game::FootstepEvent> steps;
    glm::vec3 position{0.0f};
    const glm::vec3 velocity{speed, 0.0f, 0.0f};
    // Seed the accumulator: the first update always adopts the position in
    // silence, because it cannot know how the figure got there.
    game::update_footsteps(state, position, velocity, true, crouching);
    const float per_frame = speed * dt;
    for (float walked = 0.0f; walked < distance; walked += per_frame) {
        position.x += per_frame;
        const game::FootstepEvent event =
            game::update_footsteps(state, position, velocity, true, crouching);
        if (event.stepped) {
            steps.push_back(event);
        }
    }
    return steps;
}

// Mean step gain over a long walk. The mean rather than one sample, because
// every step carries level jitter.
float mean_gain(float speed, bool crouching) {
    game::FootstepState state;
    const auto steps =
        walk(state, speed, 200.5f * game::kFootsteps.stride_meters, 1.0f / 60.0f, crouching);
    float total = 0.0f;
    for (const game::FootstepEvent& step : steps) {
        total += step.gain;
    }
    return steps.empty() ? 0.0f : total / static_cast<float>(steps.size());
}

TEST_CASE("steps come from distance, not from elapsed time", "[footsteps]") {
    // The same twelve and a half strides of ground, covered at three speeds
    // and sampled at three frame rates. Only the distance decides.
    const float ground = 12.5f * game::kFootsteps.stride_meters;
    for (const float speed : {2.0f, 6.0f, 9.3f}) {
        for (const float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 240.0f}) {
            game::FootstepState state;
            CHECK(walk(state, speed, ground, dt).size() == 12);
        }
    }
}

TEST_CASE("cadence matches the stride maths at every gait", "[footsteps]") {
    // 2.5 steps a second at the 6 m/s top speed: exactly half the run clip's
    // footfall rate, which is where the 2.4 m stride came from. The half
    // stride of extra ground keeps the count off a floating-point boundary.
    const float ground = 200.5f * game::kFootsteps.stride_meters;
    const auto rate = [ground](float speed) {
        game::FootstepState state;
        const auto steps = walk(state, speed, ground);
        return static_cast<float>(steps.size()) / (ground / speed);
    };

    CHECK(rate(game::kMove.max_speed) == Approx(2.5f).margin(0.02f));
    CHECK(rate(game::kMove.max_speed * game::kMove.sprint_multiplier) ==
          Approx(3.875f).margin(0.03f));
    CHECK(rate(game::kMove.max_speed * game::kMove.crouch_multiplier) ==
          Approx(1.125f).margin(0.02f));
}

TEST_CASE("standing still is silent however long you stand there", "[footsteps]") {
    game::FootstepState state;
    const glm::vec3 spot{4.0f, 0.0f, -3.0f};
    for (int frame = 0; frame < 600; ++frame) {
        CHECK_FALSE(game::update_footsteps(state, spot, glm::vec3{0.0f}, true, false).stepped);
    }
}

TEST_CASE("nothing fires while airborne, and the stride restarts on landing", "[footsteps]") {
    game::FootstepState state;
    glm::vec3 position{0.0f};
    const glm::vec3 velocity{6.0f, 0.0f, 0.0f};
    game::update_footsteps(state, position, velocity, true, false);

    // Almost a full stride of run-up, then leave the ground.
    position.x += game::kFootsteps.stride_meters * 0.9f;
    CHECK_FALSE(game::update_footsteps(state, position, velocity, true, false).stepped);
    for (int frame = 0; frame < 30; ++frame) {
        position.x += 0.1f;
        CHECK_FALSE(game::update_footsteps(state, position, velocity, false, false).stepped);
    }

    // Touch down too gently to thud, then walk the 0.9 stride that was banked
    // before take-off. It must NOT step: the landing reset the count.
    CHECK_FALSE(game::update_footsteps(state, position, velocity, true, false).landed);
    for (int frame = 0; frame < 9; ++frame) {
        position.x += game::kFootsteps.stride_meters * 0.1f;
        CHECK_FALSE(game::update_footsteps(state, position, velocity, true, false).stepped);
    }
    position.x += game::kFootsteps.stride_meters * 0.15f;
    CHECK(game::update_footsteps(state, position, velocity, true, false).stepped);
}

TEST_CASE("landing scales with impact speed and ignores small drops", "[footsteps]") {
    const auto land_from = [](float fall_speed) {
        game::FootstepState state;
        const glm::vec3 spot{0.0f};
        game::update_footsteps(state, spot, glm::vec3{0.0f}, true, false);
        // Airborne for a few frames at the given descent rate, held over one
        // spot so nothing here can be mistaken for a stride.
        for (int frame = 0; frame < 5; ++frame) {
            game::update_footsteps(state, spot, glm::vec3{0.0f, -fall_speed, 0.0f}, false, false);
        }
        // The controller has already cancelled the fall by the time the
        // ground comes back, which is exactly why the peak is remembered.
        return game::update_footsteps(state, spot, glm::vec3{0.0f}, true, false);
    };

    CHECK_FALSE(land_from(2.0f).landed);               // off a 0.1 m lip
    CHECK_FALSE(land_from(3.9f).landed);               // just under a 0.4 m drop
    const game::FootstepEvent jump = land_from(7.0f);  // a full jump
    CHECK(jump.landed);
    CHECK_FALSE(jump.stepped);  // a landing is not also a step
    const game::FootstepEvent plunge = land_from(14.0f);
    CHECK(plunge.land_gain > jump.land_gain);
    CHECK(plunge.land_gain == Approx(game::kFootsteps.land_max_gain));
}

TEST_CASE("crouching is quiet and sprinting is loud", "[footsteps]") {
    const float crouched = mean_gain(game::kMove.max_speed * game::kMove.crouch_multiplier, true);
    const float running = mean_gain(game::kMove.max_speed, false);
    const float sprinting = mean_gain(game::kMove.max_speed * game::kMove.sprint_multiplier, false);

    CHECK(crouched < running);
    CHECK(running < sprinting);
    // The trade has to be worth making in both directions, so the gap is
    // asserted, not just the order: a crouching player is a third the volume
    // of a running one and half again slower, and a sprinter buys speed by
    // being heard from several times the distance.
    CHECK(crouched < running * 0.4f);
    CHECK(sprinting > running * 1.5f);
}

TEST_CASE("no sample repeats back to back", "[footsteps]") {
    game::FootstepState state;
    const auto steps = walk(state, game::kMove.max_speed, 300.5f * game::kFootsteps.stride_meters);
    REQUIRE(steps.size() == 300);

    std::vector<int> used(game::kFootstepVariants, 0);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        REQUIRE(steps[i].variant < game::kFootstepVariants);
        used[steps[i].variant] += 1;
        if (i > 0) {
            CHECK(steps[i].variant != steps[i - 1].variant);
        }
        // Jitter stays inside the band the config promises, so a step can
        // never leave the gait it belongs to.
        CHECK(steps[i].pitch > 1.0f - game::kFootsteps.pitch_jitter);
        CHECK(steps[i].pitch < 1.0f + game::kFootsteps.pitch_jitter);
    }
    // Every sample gets used; a rotation that starved one would waste it.
    for (const int count : used) {
        CHECK(count > 0);
    }
}

TEST_CASE("a respawn is not a forty-metre stride", "[footsteps]") {
    game::FootstepState state;
    const glm::vec3 velocity{6.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
    game::update_footsteps(state, position, velocity, true, false);
    position.x = 1.0f;
    game::update_footsteps(state, position, velocity, true, false);

    // Killed, and put back on the far side of the arena.
    glm::vec3 elsewhere{-38.0f, 0.0f, 12.0f};
    const game::FootstepEvent teleport =
        game::update_footsteps(state, elsewhere, velocity, true, false);
    CHECK_FALSE(teleport.stepped);
    CHECK_FALSE(teleport.landed);

    // And the banked metre did not survive the move.
    for (int frame = 0; frame < 13; ++frame) {
        elsewhere.x += 0.1f;
        CHECK_FALSE(game::update_footsteps(state, elsewhere, velocity, true, false).stepped);
    }
}

TEST_CASE("only horizontal travel counts", "[footsteps]") {
    // Riding a platform, or being pushed up out of geometry, is not walking.
    game::FootstepState state;
    glm::vec3 position{0.0f};
    const glm::vec3 velocity{0.0f, 3.0f, 0.0f};
    game::update_footsteps(state, position, velocity, true, false);
    for (int frame = 0; frame < 200; ++frame) {
        position.y += 0.05f;
        CHECK_FALSE(game::update_footsteps(state, position, velocity, true, false).stepped);
    }
}

}  // namespace
