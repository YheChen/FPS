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

}  // namespace
