#include "engine/scene/scene.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/quaternion.hpp>

namespace {

TEST_CASE("scene create/get/destroy with generational safety", "[scene]") {
    eng::Scene scene;
    const eng::EntityId a = scene.create("a");
    const eng::EntityId b = scene.create("b");
    CHECK(scene.count() == 2);
    CHECK(scene.get(a)->name == "a");
    CHECK(scene.get(b)->name == "b");

    scene.destroy(a);
    CHECK(scene.count() == 1);
    CHECK(scene.get(a) == nullptr);  // stale ID rejected
    CHECK_FALSE(scene.alive(a));

    // Slot reuse must not resurrect the old ID.
    const eng::EntityId c = scene.create("c");
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(scene.get(a) == nullptr);
    CHECK(scene.get(c)->name == "c");

    // Double destroy is a no-op.
    scene.destroy(a);
    CHECK(scene.count() == 2);
}

TEST_CASE("scene iteration only visits alive entities", "[scene]") {
    eng::Scene scene;
    scene.create("one");
    const eng::EntityId dead = scene.create("two");
    scene.create("three");
    scene.destroy(dead);

    int visited = 0;
    scene.each([&](eng::EntityId, eng::Entity& entity) {
        CHECK(entity.name != "two");
        ++visited;
    });
    CHECK(visited == 2);
}

TEST_CASE("transform TRS round-trips through a matrix", "[scene]") {
    eng::Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3{0.0f, 0.0f, 1.0f});
    t.scale = {2.0f, 4.0f, 0.5f};

    const eng::Transform back = eng::Transform::from_matrix(t.to_matrix());
    CHECK(back.position.x == Catch::Approx(1.0f));
    CHECK(back.position.y == Catch::Approx(2.0f));
    CHECK(back.position.z == Catch::Approx(3.0f));
    CHECK(back.scale.x == Catch::Approx(2.0f));
    CHECK(back.scale.y == Catch::Approx(4.0f));
    CHECK(back.scale.z == Catch::Approx(0.5f));
    // Quaternion may flip sign; compare the rotation action instead.
    const glm::vec3 v{1.0f, 0.0f, 0.0f};
    const glm::vec3 expect = t.rotation * v;
    const glm::vec3 actual = back.rotation * v;
    CHECK(actual.x == Catch::Approx(expect.x).margin(1e-5));
    CHECK(actual.y == Catch::Approx(expect.y).margin(1e-5));
    CHECK(actual.z == Catch::Approx(expect.z).margin(1e-5));
}

}  // namespace
