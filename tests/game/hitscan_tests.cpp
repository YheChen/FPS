#include "game/shared/hitscan.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/shared/health.h"

namespace {

using Catch::Approx;

TEST_CASE("ray_sphere hits, misses, and handles inside/behind", "[hitscan]") {
    const glm::vec3 center{0.0f, 0.0f, -10.0f};

    const auto hit = game::ray_sphere({0, 0, 0}, {0, 0, -1}, center, 1.0f);
    REQUIRE(hit.has_value());
    CHECK(*hit == Approx(9.0f));

    CHECK_FALSE(game::ray_sphere({0, 0, 0}, {0, 0, -1}, {5, 0, -10}, 1.0f).has_value());
    CHECK_FALSE(game::ray_sphere({0, 0, 0}, {0, 0, 1}, center, 1.0f).has_value());  // behind

    const auto inside = game::ray_sphere(center, {0, 0, -1}, center, 1.0f);
    REQUIRE(inside.has_value());
    CHECK(*inside == 0.0f);
}

TEST_CASE("ray_vertical_capsule hits the cylinder wall", "[hitscan]") {
    // Capsule: feet at origin, radius 0.4, height 1.8.
    const auto hit =
        game::ray_vertical_capsule({-5.0f, 0.9f, 0.0f}, {1, 0, 0}, {0, 0, 0}, 0.4f, 1.8f);
    REQUIRE(hit.has_value());
    CHECK(*hit == Approx(4.6f).margin(1e-3f));
}

TEST_CASE("ray_vertical_capsule hits the top cap and misses above it", "[hitscan]") {
    // Straight down onto the head: top cap sphere center y=1.4, radius 0.4.
    const auto head =
        game::ray_vertical_capsule({0.0f, 5.0f, 0.0f}, {0, -1, 0}, {0, 0, 0}, 0.4f, 1.8f);
    REQUIRE(head.has_value());
    CHECK(*head == Approx(5.0f - 1.8f).margin(1e-3f));

    // A ray passing 0.5 m above the head misses.
    const auto above =
        game::ray_vertical_capsule({-5.0f, 2.3f, 0.0f}, {1, 0, 0}, {0, 0, 0}, 0.4f, 1.8f);
    CHECK_FALSE(above.has_value());
}

TEST_CASE("ray_vertical_capsule respects horizontal offset", "[hitscan]") {
    // Ray offset 0.3 in z still clips the r=0.4 capsule; 0.5 misses.
    CHECK(game::ray_vertical_capsule({-5.0f, 0.9f, 0.3f}, {1, 0, 0}, {0, 0, 0}, 0.4f, 1.8f)
              .has_value());
    CHECK_FALSE(game::ray_vertical_capsule({-5.0f, 0.9f, 0.5f}, {1, 0, 0}, {0, 0, 0}, 0.4f, 1.8f)
                    .has_value());
}

TEST_CASE("view_direction matches the camera convention", "[hitscan]") {
    const glm::vec3 ahead = game::view_direction(0.0f, 0.0f);
    CHECK(ahead.x == Approx(0.0f).margin(1e-6f));
    CHECK(ahead.z == Approx(-1.0f));

    const glm::vec3 right = game::view_direction(glm::radians(90.0f), 0.0f);
    CHECK(right.x == Approx(1.0f));

    const glm::vec3 up45 = game::view_direction(0.0f, glm::radians(45.0f));
    CHECK(up45.y == Approx(std::sin(glm::radians(45.0f))));
}

TEST_CASE("damage and death transitions", "[health]") {
    game::Health health{50.0f, 50.0f};
    CHECK_FALSE(game::apply_damage(health, 20.0f));  // 30 left
    CHECK_FALSE(game::apply_damage(health, 20.0f));  // 10 left
    CHECK(health.alive());
    CHECK(game::apply_damage(health, 25.0f));  // dies exactly once
    CHECK_FALSE(health.alive());
    CHECK(health.current == 0.0f);
    CHECK_FALSE(game::apply_damage(health, 25.0f));  // dead stays dead
    game::reset_health(health);
    CHECK(health.alive());
    CHECK(health.current == 50.0f);
}

// The standing player the zone tests shoot at: the character controller's
// real proportions (see engine/physics/character_controller.h).
constexpr float kRadius = 0.4f;
constexpr float kHeight = 1.8f;

// A shot fired horizontally from 5 m away along +X, passing `sideways` metres
// to the side of the target's axis, striking at height `y` above its feet.
game::HitZone zone_of_shot(const glm::vec3& feet, float y, float sideways, float height = kHeight,
                           float radius = kRadius) {
    const glm::vec3 origin{feet.x - 5.0f, feet.y + y, feet.z + sideways};
    const glm::vec3 direction{1.0f, 0.0f, 0.0f};
    // Where that ray meets the near face of the capsule.
    const glm::vec3 impact{feet.x - radius, feet.y + y, feet.z + sideways};
    return game::classify_hit_zone(impact, origin, direction, feet, radius, height);
}

TEST_CASE("hit zones split a capsule into head, torso, arms and legs", "[hitscan]") {
    const glm::vec3 feet{0.0f, 0.0f, 0.0f};

    CHECK(zone_of_shot(feet, 1.70f, 0.0f) == game::HitZone::Head);   // above the neck
    CHECK(zone_of_shot(feet, 1.20f, 0.0f) == game::HitZone::Torso);  // centre mass
    CHECK(zone_of_shot(feet, 0.50f, 0.0f) == game::HitZone::Leg);    // below the hip

    // Chest height, but the ray passes out by the edge of the silhouette.
    CHECK(zone_of_shot(feet, 1.20f, 0.30f) == game::HitZone::Arm);
    CHECK(zone_of_shot(feet, 1.20f, -0.30f) == game::HitZone::Arm);  // and on the other side
    // ...while a shot only slightly off centre is still body.
    CHECK(zone_of_shot(feet, 1.20f, 0.10f) == game::HitZone::Torso);
}

TEST_CASE("a torso shot is not mistaken for an arm", "[hitscan]") {
    // The regression this whole model nearly shipped with: a ray always lands
    // on the capsule's SURFACE, a full radius from the axis. Classifying by
    // the impact point's distance from the axis therefore called every hit an
    // edge graze, and centre-mass shots quietly did limb damage.
    const glm::vec3 feet{0.0f, 0.0f, 0.0f};
    CHECK(zone_of_shot(feet, 1.20f, 0.0f) == game::HitZone::Torso);
    CHECK(zone_of_shot(feet, 1.00f, 0.0f) == game::HitZone::Torso);
    CHECK(zone_of_shot(feet, 1.40f, 0.0f) == game::HitZone::Torso);
}

TEST_CASE("hit zones are relative to the player, not to the world", "[hitscan]") {
    // The same shot against a capsule standing somewhere else entirely must
    // classify the same way, or a zone would depend on where on the map the
    // fight happened.
    const glm::vec3 feet{5.0f, 2.0f, -3.0f};
    CHECK(zone_of_shot(feet, 1.70f, 0.0f) == game::HitZone::Head);
    CHECK(zone_of_shot(feet, 0.50f, 0.0f) == game::HitZone::Leg);
    CHECK(zone_of_shot(feet, 1.20f, 0.30f) == game::HitZone::Arm);
    CHECK(zone_of_shot(feet, 1.20f, 0.00f) == game::HitZone::Torso);
}

TEST_CASE("crouching lowers the head rather than removing it", "[hitscan]") {
    const glm::vec3 feet{0.0f, 0.0f, 0.0f};
    constexpr float kCrouchHeight = 1.2f;

    // A shot that is centre mass on a standing player is a HEAD shot on a
    // crouched one, because the zones are fractions of the capsule actually
    // being shot at. Getting this wrong is how headshots silently stop
    // working the moment someone crouches.
    CHECK(zone_of_shot(feet, 1.1f, 0.0f, kHeight) == game::HitZone::Torso);
    CHECK(zone_of_shot(feet, 1.1f, 0.0f, kCrouchHeight) == game::HitZone::Head);
}

TEST_CASE("hit zone classification survives degenerate input", "[hitscan]") {
    const glm::vec3 feet{0.0f, 0.0f, 0.0f};
    const glm::vec3 origin{-5.0f, 1.2f, 0.0f};
    const glm::vec3 across{1.0f, 0.0f, 0.0f};

    // A zero-height capsule has no meaningful zones and must not divide by it.
    CHECK(game::classify_hit_zone({0.0f, 0.0f, 0.0f}, origin, across, feet, kRadius, 0.0f) ==
          game::HitZone::Torso);
    // A zero radius cannot have an arm band, but still has a head and legs.
    CHECK(zone_of_shot(feet, 1.7f, 0.0f, kHeight, 0.0f) == game::HitZone::Head);
    CHECK(zone_of_shot(feet, 1.2f, 0.9f, kHeight, 0.0f) == game::HitZone::Torso);
    // Points off the ends clamp instead of running off the enum.
    CHECK(zone_of_shot(feet, 99.0f, 0.0f) == game::HitZone::Head);
    CHECK(zone_of_shot(feet, -99.0f, 0.0f) == game::HitZone::Leg);

    // A shot straight down has no lateral offset to measure and must not
    // divide by a zero-length horizontal direction.
    CHECK(game::classify_hit_zone({0.0f, 1.2f, 0.0f}, {0.0f, 9.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, feet,
                                  kRadius, kHeight) == game::HitZone::Torso);
}

TEST_CASE("ray_lateral_offset measures distance from the axis, not along it", "[hitscan]") {
    const glm::vec3 feet{0.0f, 0.0f, 0.0f};
    const glm::vec3 across{1.0f, 0.0f, 0.0f};
    // Straight at the axis from any distance: zero offset either way.
    CHECK(game::ray_lateral_offset({-5.0f, 1.0f, 0.0f}, across, feet) == Approx(0.0f).margin(1e-5));
    CHECK(game::ray_lateral_offset({-50.0f, 1.0f, 0.0f}, across, feet) ==
          Approx(0.0f).margin(1e-5));
    // Offset to the side is that side-step, regardless of range.
    CHECK(game::ray_lateral_offset({-5.0f, 1.0f, 0.25f}, across, feet) == Approx(0.25f));
    CHECK(game::ray_lateral_offset({-5.0f, 1.0f, -0.25f}, across, feet) == Approx(0.25f));
    // An unnormalized direction must not change the answer.
    CHECK(game::ray_lateral_offset({-5.0f, 1.0f, 0.25f}, {7.0f, 0.0f, 0.0f}, feet) ==
          Approx(0.25f));
}

}  // namespace
