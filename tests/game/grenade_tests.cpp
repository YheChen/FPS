#include "game/shared/grenade.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

namespace {

TEST_CASE("blast damage falls off to nothing at the edge", "[grenade]") {
    // Full at the centre, zero at the radius, linear between. Linear and not
    // inverse-square on purpose: inverse-square spends almost all of its range
    // doing nothing, which reads as a grenade that does not work rather than
    // one that missed.
    CHECK(game::grenade_damage_at(0.0f) == Approx(game::kGrenadeDamage));
    CHECK(game::grenade_damage_at(game::kGrenadeBlastRadius * 0.5f) ==
          Approx(game::kGrenadeDamage * 0.5f));
    CHECK(game::grenade_damage_at(game::kGrenadeBlastRadius) == Approx(0.0f));
    // Past the edge is zero, not negative -- a negative would HEAL through
    // apply_damage, and a grenade across the map would top people up.
    CHECK(game::grenade_damage_at(game::kGrenadeBlastRadius * 3.0f) == Approx(0.0f));
    CHECK(game::grenade_damage_at(-1.0f) == Approx(game::kGrenadeDamage));

    // A direct hit kills outright, or cooking one and dropping it at your own
    // feet would be survivable and the decision would evaporate.
    CHECK(game::grenade_damage_at(0.0f) > 100.0f);
}

TEST_CASE("a blast hurts you and your enemies but never your team", "[grenade][team]") {
    constexpr std::uint8_t kThrower = 2;
    constexpr std::uint8_t kMate = 3;
    constexpr std::uint8_t kEnemy = 4;

    // Yourself: yes. This is the whole cost of cooking. Without it, holding G
    // to the last instant is free and the mechanic is a strictly better throw.
    CHECK(game::blast_can_damage(kThrower, game::Team::A, kThrower, game::Team::A));
    // Teammate: no, for the same reason bullets do not (M52).
    CHECK_FALSE(game::blast_can_damage(kThrower, game::Team::A, kMate, game::Team::A));
    // Enemy: yes.
    CHECK(game::blast_can_damage(kThrower, game::Team::A, kEnemy, game::Team::B));
    // ...and from the other side, so the rule is not accidentally about which
    // letter is which.
    CHECK(game::blast_can_damage(kThrower, game::Team::B, kEnemy, game::Team::A));
    CHECK_FALSE(game::blast_can_damage(kThrower, game::Team::B, kMate, game::Team::B));
}

TEST_CASE("a throw inherits the thrower's run but not their fall", "[grenade]") {
    // Looking straight ahead (-Z at yaw 0, pitch 0).
    const glm::vec3 still = game::grenade_throw_velocity(0.0f, 0.0f, {0.0f, 0.0f, 0.0f});
    CHECK(still.z == Approx(-game::kGrenadeThrowSpeed));
    CHECK(still.y == Approx(0.0f));

    // Running forward adds to it, so a grenade thrown at a sprint does not
    // hang in the air behind the person who threw it.
    const glm::vec3 running = game::grenade_throw_velocity(0.0f, 0.0f, {0.0f, 0.0f, -6.0f});
    CHECK(running.z == Approx(-game::kGrenadeThrowSpeed - 6.0f));

    // Falling does NOT. Inheriting it would make a grenade thrown off a ledge
    // drop like a stone, which reads as the throw failing rather than as
    // physics.
    const glm::vec3 falling = game::grenade_throw_velocity(0.0f, 0.0f, {0.0f, -20.0f, 0.0f});
    CHECK(falling.y == Approx(0.0f));

    // Aiming up puts it up: the arc is the player's, not a fixed lob.
    const glm::vec3 lobbed = game::grenade_throw_velocity(0.0f, 0.7f, {0.0f, 0.0f, 0.0f});
    CHECK(lobbed.y > 0.0f);
}

TEST_CASE("the launch arc reaches, and says when it cannot", "[grenade]") {
    // A grenade is not a bullet: aiming AT something misses it. At the throw
    // speed a level throw from eye height is on the floor after about six
    // metres, so anything trying to land one has to solve the arc.
    const auto near = game::grenade_launch_pitch(2.0f);
    const auto far = game::grenade_launch_pitch(10.0f);
    REQUIRE(near.has_value());
    REQUIRE(far.has_value());
    // Further needs more elevation, and both are the LOW arc -- under 45
    // degrees. The high solution lobs over cover and hangs long enough for
    // anyone to walk away, which is the wrong grenade almost every time.
    CHECK(*far > *near);
    CHECK(*far < 0.7854f);
    CHECK(*near > 0.0f);

    // Past v^2/g there is no angle at all, and this says so rather than
    // quietly falling short.
    const float max_reach =
        game::kGrenadeThrowSpeed * game::kGrenadeThrowSpeed / game::kGrenadeGravity;
    CHECK(game::grenade_launch_pitch(max_reach * 0.99f).has_value());
    CHECK_FALSE(game::grenade_launch_pitch(max_reach * 1.05f).has_value());

    // Zero distance is a throw at your own feet: legal, and level.
    CHECK(game::grenade_launch_pitch(0.0f) == 0.0f);
}

}  // namespace
