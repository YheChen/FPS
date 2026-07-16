#include "engine/rendering/camera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace {

using Catch::Approx;

TEST_CASE("camera looks down -Z at rest", "[camera]") {
    const eng::Camera camera{};
    const glm::vec3 f = camera.forward();
    CHECK(f.x == Approx(0.0f).margin(1e-6));
    CHECK(f.y == Approx(0.0f).margin(1e-6));
    CHECK(f.z == Approx(-1.0f).margin(1e-6));
}

TEST_CASE("positive yaw turns right toward +X", "[camera]") {
    eng::Camera camera{};
    camera.yaw = glm::half_pi<float>();
    const glm::vec3 f = camera.forward();
    CHECK(f.x == Approx(1.0f).margin(1e-6));
    CHECK(f.z == Approx(0.0f).margin(1e-6));

    const glm::vec3 r = camera.right();
    CHECK(r.z == Approx(1.0f).margin(1e-6));  // right of +X facing is +Z
}

TEST_CASE("positive pitch looks up", "[camera]") {
    eng::Camera camera{};
    camera.pitch = glm::radians(45.0f);
    const glm::vec3 f = camera.forward();
    CHECK(f.y == Approx(std::sin(glm::radians(45.0f))));
    CHECK(f.z < 0.0f);
}

TEST_CASE("clamp_angles limits pitch and wraps yaw", "[camera]") {
    eng::Camera camera{};
    camera.pitch = glm::radians(120.0f);
    camera.yaw = glm::radians(370.0f);
    camera.clamp_angles();
    CHECK(camera.pitch == Approx(glm::radians(89.0f)));
    CHECK(camera.yaw == Approx(glm::radians(10.0f)).margin(1e-5));
}

TEST_CASE("view transforms the camera position to the origin", "[camera]") {
    eng::Camera camera{};
    camera.position = {3.0f, 2.0f, -5.0f};
    camera.yaw = 0.7f;
    camera.pitch = -0.3f;
    const glm::vec4 eye = camera.view() * glm::vec4(camera.position, 1.0f);
    CHECK(eye.x == Approx(0.0f).margin(1e-5));
    CHECK(eye.y == Approx(0.0f).margin(1e-5));
    CHECK(eye.z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("projection encodes the vertical field of view", "[camera]") {
    eng::Camera camera{};
    camera.fov_y_degrees = 90.0f;
    camera.aspect = 2.0f;
    const glm::mat4 p = camera.projection();
    CHECK(p[1][1] == Approx(1.0f));  // 1/tan(45 deg)
    CHECK(p[0][0] == Approx(0.5f));  // divided by aspect
}

}  // namespace
