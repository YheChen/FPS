#include "engine/rendering/light.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

namespace {

// Projects a world point through a light matrix into NDC.
glm::vec3 to_ndc(const glm::mat4& matrix, const glm::vec3& point) {
    const glm::vec4 clip = matrix * glm::vec4(point, 1.0f);
    return glm::vec3(clip) / clip.w;
}

bool inside_ndc(const glm::vec3& ndc, float slack = 1e-4f) {
    return ndc.x >= -1.0f - slack && ndc.x <= 1.0f + slack && ndc.y >= -1.0f - slack &&
           ndc.y <= 1.0f + slack && ndc.z >= -1.0f - slack && ndc.z <= 1.0f + slack;
}

eng::Bounds arena_like() {
    eng::Bounds bounds;
    bounds.expand({-20.0f, -1.0f, -20.0f});
    bounds.expand({20.0f, 4.5f, 20.0f});
    return bounds;
}

TEST_CASE("bounds start empty and grow to contain points", "[light]") {
    eng::Bounds bounds;
    CHECK(bounds.empty());

    bounds.expand({1.0f, 2.0f, 3.0f});
    CHECK_FALSE(bounds.empty());
    CHECK(bounds.center().x == Catch::Approx(1.0f));
    CHECK(bounds.extent().y == Catch::Approx(0.0f));

    bounds.expand({-1.0f, 6.0f, 3.0f});
    CHECK(bounds.min.x == Catch::Approx(-1.0f));
    CHECK(bounds.max.y == Catch::Approx(6.0f));
    CHECK(bounds.center().y == Catch::Approx(4.0f));
    CHECK(bounds.extent().x == Catch::Approx(2.0f));
}

TEST_CASE("bounds expand by a transformed box", "[light]") {
    eng::Bounds unit;
    unit.expand({-0.5f, -0.5f, -0.5f});
    unit.expand({0.5f, 0.5f, 0.5f});

    // A unit cube scaled 40x and lifted, the way an arena floor node is.
    glm::mat4 transform = glm::translate(glm::mat4{1.0f}, {0.0f, -0.5f, 0.0f});
    transform = glm::scale(transform, {40.0f, 1.0f, 40.0f});

    eng::Bounds world;
    world.expand(unit, transform);
    CHECK(world.min.x == Catch::Approx(-20.0f));
    CHECK(world.max.x == Catch::Approx(20.0f));
    CHECK(world.min.y == Catch::Approx(-1.0f));
    CHECK(world.max.y == Catch::Approx(0.0f));

    // Expanding by an empty box is a no-op, not a corruption.
    const eng::Bounds before = world;
    world.expand(eng::Bounds{}, glm::mat4{1.0f});
    CHECK(world.min.x == Catch::Approx(before.min.x));
    CHECK(world.max.y == Catch::Approx(before.max.y));
}

TEST_CASE("light projection covers every corner of the bounds", "[light]") {
    const eng::Bounds bounds = arena_like();

    // Several sun angles, including straight down (where a naive lookAt with
    // a +Y up vector degenerates).
    const glm::vec3 directions[] = {
        {-0.4f, -1.0f, -0.3f}, {0.0f, -1.0f, 0.0f},  {1.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 1.0f},   {-0.2f, -0.4f, 0.9f}, {0.0f, 1.0f, 0.0f},
    };

    for (const glm::vec3& direction : directions) {
        const glm::mat4 matrix = eng::directional_light_view_projection(direction, bounds);
        for (const glm::vec3& corner : bounds.corners()) {
            const glm::vec3 ndc = to_ndc(matrix, corner);
            INFO("direction " << direction.x << "," << direction.y << "," << direction.z);
            CHECK(inside_ndc(ndc));
        }
    }
}

TEST_CASE("light projection is fitted, not merely covering", "[light]") {
    const eng::Bounds bounds = arena_like();
    const glm::mat4 matrix = eng::directional_light_view_projection({0.0f, -1.0f, 0.0f}, bounds);

    // A tight fit means the extremes actually reach the edges of NDC. A
    // projection that covered twice the needed area would pass the
    // containment test above while wasting half the shadow map per axis.
    float max_abs_x = 0.0f;
    float max_abs_y = 0.0f;
    for (const glm::vec3& corner : bounds.corners()) {
        const glm::vec3 ndc = to_ndc(matrix, corner);
        max_abs_x = std::max(max_abs_x, std::abs(ndc.x));
        max_abs_y = std::max(max_abs_y, std::abs(ndc.y));
    }
    CHECK(max_abs_x == Catch::Approx(1.0f).margin(1e-4f));
    CHECK(max_abs_y == Catch::Approx(1.0f).margin(1e-4f));
}

TEST_CASE("a point above the bounds still projects inside the map", "[light]") {
    // Players stand on top of the geometry, so the caster volume has to
    // include headroom the static bounds do not.
    eng::Bounds bounds = arena_like();
    bounds.max.y += 2.5f;
    const glm::mat4 matrix = eng::directional_light_view_projection({-0.4f, -1.0f, -0.3f}, bounds);

    const glm::vec3 head{0.0f, 6.0f, 0.0f};  // on the center platform
    CHECK(inside_ndc(to_ndc(matrix, head)));
}

TEST_CASE("degenerate inputs fall back to identity", "[light]") {
    const eng::Bounds empty;
    CHECK(eng::directional_light_view_projection({0.0f, -1.0f, 0.0f}, empty) == glm::mat4{1.0f});
    CHECK(eng::directional_light_view_projection({0.0f, 0.0f, 0.0f}, arena_like()) ==
          glm::mat4{1.0f});
}

}  // namespace
