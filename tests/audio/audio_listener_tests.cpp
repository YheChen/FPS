#include "engine/audio/audio_listener.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "engine/rendering/camera.h"

// The point of these tests is the SIGN of the x axis. Everything else about
// spatial audio degrades gracefully -- a wrong rolloff is a mixing complaint --
// but a mirrored listener frame tells players to turn toward the shot when
// they should turn away, and nothing in a build log or a screenshot would
// catch it.
namespace {

using Catch::Approx;

constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
constexpr glm::vec3 kForward{0.0f, 0.0f, -1.0f};  // yaw 0, pitch 0

TEST_CASE("listener basis is orthonormal and right-handed", "[audio]") {
    const eng::ListenerBasis basis = eng::listener_basis(glm::vec3{0.4f, 0.2f, -0.9f}, kUp);
    CHECK(glm::length(basis.right) == Approx(1.0f));
    CHECK(glm::length(basis.up) == Approx(1.0f));
    CHECK(glm::length(basis.forward) == Approx(1.0f));
    CHECK(glm::dot(basis.right, basis.up) == Approx(0.0f).margin(1e-6));
    CHECK(glm::dot(basis.right, basis.forward) == Approx(0.0f).margin(1e-6));
    CHECK(glm::dot(basis.up, basis.forward) == Approx(0.0f).margin(1e-6));
    // Right-handed with -Z forward: right x up = -forward.
    const glm::vec3 handed = glm::cross(basis.right, basis.up);
    CHECK(handed.x == Approx(-basis.forward.x).margin(1e-6));
    CHECK(handed.y == Approx(-basis.forward.y).margin(1e-6));
    CHECK(handed.z == Approx(-basis.forward.z).margin(1e-6));
}

TEST_CASE("listener basis agrees with the camera it is driven from", "[audio]") {
    eng::Camera camera;
    camera.yaw = 0.9f;
    camera.pitch = -0.25f;
    const eng::ListenerBasis basis = eng::listener_basis(camera.forward(), kUp);
    const glm::vec3 camera_right = camera.right();
    CHECK(basis.right.x == Approx(camera_right.x).margin(1e-6));
    CHECK(basis.right.y == Approx(camera_right.y).margin(1e-6));
    CHECK(basis.right.z == Approx(camera_right.z).margin(1e-6));
}

TEST_CASE("a sound at +X is on the right when facing -Z", "[audio]") {
    const glm::vec3 local = eng::listener_space({}, kForward, kUp, {10.0f, 0.0f, 0.0f});
    CHECK(local.x == Approx(10.0f));  // positive x == right speaker
    CHECK(local.y == Approx(0.0f).margin(1e-6));
    CHECK(local.z == Approx(0.0f).margin(1e-6));
}

TEST_CASE("straight ahead is -Z in listener space, behind is +Z", "[audio]") {
    const glm::vec3 ahead = eng::listener_space({}, kForward, kUp, {0.0f, 0.0f, -7.0f});
    CHECK(ahead.z == Approx(-7.0f));
    CHECK(ahead.x == Approx(0.0f).margin(1e-6));

    const glm::vec3 behind = eng::listener_space({}, kForward, kUp, {0.0f, 0.0f, 7.0f});
    CHECK(behind.z == Approx(7.0f));
    CHECK(behind.x == Approx(0.0f).margin(1e-6));
}

TEST_CASE("turning right moves a sound to the left ear", "[audio]") {
    // Face +X (yaw +90 degrees). A sound still sitting at world +X is now
    // dead ahead, and one at world -Z -- which used to be ahead -- is on the
    // left. Get the sign backwards and this test flips both.
    eng::Camera camera;
    camera.yaw = glm::half_pi<float>();
    const glm::vec3 forward = camera.forward();

    const glm::vec3 ahead = eng::listener_space({}, forward, kUp, {10.0f, 0.0f, 0.0f});
    CHECK(ahead.z == Approx(-10.0f).margin(1e-5));
    CHECK(ahead.x == Approx(0.0f).margin(1e-5));

    const glm::vec3 left = eng::listener_space({}, forward, kUp, {0.0f, 0.0f, -10.0f});
    CHECK(left.x == Approx(-10.0f).margin(1e-5));
    CHECK(left.z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("listener position is subtracted before rotation", "[audio]") {
    const glm::vec3 listener{3.0f, 1.6f, -4.0f};
    const glm::vec3 local = eng::listener_space(listener, kForward, kUp, listener);
    CHECK(glm::length(local) == Approx(0.0f).margin(1e-6));

    // Emitter two meters to the world east and three ahead of the listener.
    const glm::vec3 offset =
        eng::listener_space(listener, kForward, kUp, listener + glm::vec3{2.0f, 0.0f, -3.0f});
    CHECK(offset.x == Approx(2.0f));
    CHECK(offset.z == Approx(-3.0f));
}

TEST_CASE("pitch does not leak into the left/right axis", "[audio]") {
    // Right is cross(forward, world_up), so it stays horizontal however far
    // the view tips. A sound directly overhead must be dead centre, not
    // panned -- looking up should not make the world lean.
    eng::Camera camera;
    camera.pitch = glm::radians(60.0f);
    const eng::ListenerBasis basis = eng::listener_basis(camera.forward(), kUp);
    CHECK(basis.right.y == Approx(0.0f).margin(1e-6));

    const glm::vec3 overhead = eng::listener_space({}, camera.forward(), kUp, {0.0f, 5.0f, 0.0f});
    CHECK(overhead.x == Approx(0.0f).margin(1e-6));
}

TEST_CASE("looking straight up falls back to +X instead of NaN", "[audio]") {
    // cross(forward, up) collapses here. miniaudio substitutes +X rather than
    // dividing by zero, and the panning maths must agree with it.
    const eng::ListenerBasis basis = eng::listener_basis(kUp, kUp);
    CHECK(basis.right.x == Approx(1.0f));
    CHECK(basis.right.y == Approx(0.0f).margin(1e-6));
    CHECK(basis.right.z == Approx(0.0f).margin(1e-6));

    const glm::vec3 local = eng::listener_space({}, kUp, kUp, {4.0f, 0.0f, 0.0f});
    CHECK(local.x == Approx(4.0f));
}

TEST_CASE("a degenerate forward vector does not produce NaN", "[audio]") {
    const eng::ListenerBasis basis = eng::listener_basis(glm::vec3{0.0f}, kUp);
    CHECK(basis.forward.z == Approx(-1.0f));
    CHECK(basis.right.x == Approx(1.0f));
    CHECK(basis.up.y == Approx(1.0f));
}

TEST_CASE("world up need not be perpendicular to forward", "[audio]") {
    // miniaudio normalizes the side vector precisely because callers hand it
    // a world up that is not square to the view; a tilted-but-unnormalized up
    // must not stretch the frame.
    const eng::ListenerBasis basis = eng::listener_basis(kForward, glm::vec3{0.0f, 3.0f, -0.5f});
    CHECK(glm::length(basis.right) == Approx(1.0f));
    CHECK(basis.right.x == Approx(1.0f));
}

}  // namespace
