#include "game/shared/player_movement.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

#include "game/shared/input_command.h"

namespace {

using Catch::Approx;

// Flat 40x40 m floor with its top surface at y = 0.
struct TestArena {
    eng::PhysicsWorld world;

    TestArena() {
        world.add_static_box({0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
        world.optimize();
    }
};

game::InputCommand command_with(std::uint8_t buttons, float yaw = 0.0f) {
    game::InputCommand command;
    command.buttons = buttons;
    command.yaw = yaw;
    return command;
}

constexpr std::uint8_t kForward = static_cast<std::uint8_t>(game::Button::Forward);
constexpr std::uint8_t kJump = static_cast<std::uint8_t>(game::Button::Jump);

game::PlayerState simulate(TestArena& arena, const std::vector<game::InputCommand>& commands,
                           const glm::vec3& start) {
    eng::CharacterController controller{arena.world, start};
    game::PlayerState state;
    state.position = start;
    for (const game::InputCommand& command : commands) {
        game::advance_player(state, command, game::kTickSeconds, controller, arena.world);
    }
    return state;
}

TEST_CASE("player falls to the floor and lands", "[movement]") {
    TestArena arena;
    const std::vector<game::InputCommand> idle(120);
    const game::PlayerState state = simulate(arena, idle, {0.0f, 3.0f, 0.0f});
    CHECK(state.on_ground);
    CHECK(state.position.y == Approx(0.0f).margin(0.02f));
}

TEST_CASE("input command button helpers round-trip", "[movement]") {
    game::InputCommand command;
    game::set_button(command, game::Button::Jump, true);
    game::set_button(command, game::Button::Fire, true);
    CHECK(game::has_button(command, game::Button::Jump));
    CHECK(game::has_button(command, game::Button::Fire));
    CHECK_FALSE(game::has_button(command, game::Button::Forward));
    game::set_button(command, game::Button::Jump, false);
    CHECK_FALSE(game::has_button(command, game::Button::Jump));
}

TEST_CASE("horizontal speed is capped at max_speed", "[movement]") {
    TestArena arena;
    std::vector<game::InputCommand> commands(30);  // settle
    for (int i = 0; i < 180; ++i) {
        commands.push_back(command_with(kForward));  // 3 s of full throttle
    }
    const game::PlayerState state = simulate(arena, commands, {0.0f, 0.1f, 0.0f});
    const float horizontal = std::hypot(state.velocity.x, state.velocity.z);
    CHECK(horizontal <= game::kMove.max_speed + 0.01f);
    CHECK(horizontal >= game::kMove.max_speed * 0.95f);
    // yaw=0 faces -Z: forward motion decreases z.
    CHECK(state.position.z < -1.0f);
}

TEST_CASE("jump reaches the configured apex height", "[movement]") {
    TestArena arena;
    eng::CharacterController controller{arena.world, {0.0f, 0.1f, 0.0f}};
    game::PlayerState state;
    state.position = {0.0f, 0.1f, 0.0f};

    // Settle onto the floor first.
    for (int i = 0; i < 30; ++i) {
        game::advance_player(state, {}, game::kTickSeconds, controller, arena.world);
    }
    REQUIRE(state.on_ground);

    float max_height = 0.0f;
    bool jumped = false;
    for (int i = 0; i < 120; ++i) {
        const game::InputCommand command = command_with(i == 0 ? kJump : 0);
        game::advance_player(state, command, game::kTickSeconds, controller, arena.world);
        if (!state.on_ground) {
            jumped = true;
        }
        max_height = std::max(max_height, state.position.y);
    }
    REQUIRE(jumped);
    // Discrete integration: apex = v^2/2g plus up to one tick of drift.
    const float expected =
        game::kMove.jump_speed * game::kMove.jump_speed / (2.0f * game::kMove.gravity);
    CHECK(max_height == Approx(expected).margin(0.15f));
    CHECK(state.on_ground);  // landed again
}

TEST_CASE("movement simulation is deterministic across runs", "[movement]") {
    // A varied input script: run, turn, jump, stop.
    std::vector<game::InputCommand> script;
    for (int i = 0; i < 240; ++i) {
        game::InputCommand command;
        command.yaw = 0.01f * static_cast<float>(i % 100);
        if (i % 3 != 0) {
            game::set_button(command, game::Button::Forward, true);
        }
        if (i % 50 == 10) {
            game::set_button(command, game::Button::Jump, true);
        }
        if (i % 7 == 0) {
            game::set_button(command, game::Button::Right, true);
        }
        script.push_back(command);
    }

    TestArena arena_a;
    TestArena arena_b;
    const game::PlayerState a = simulate(arena_a, script, {1.0f, 0.5f, 2.0f});
    const game::PlayerState b = simulate(arena_b, script, {1.0f, 0.5f, 2.0f});

    // Bit-exact equality: prediction replay (M7) depends on this.
    CHECK(std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0);
    CHECK(std::memcmp(&a.velocity, &b.velocity, sizeof(a.velocity)) == 0);
    CHECK(a.on_ground == b.on_ground);
}

TEST_CASE("sprinting raises top speed, crouching lowers it", "[movement]") {
    constexpr std::uint8_t kSprint = static_cast<std::uint8_t>(game::Button::Sprint);
    constexpr std::uint16_t kCrouch = static_cast<std::uint16_t>(game::Button::Crouch);

    const auto settle_and_run = [](std::uint16_t extra_buttons) {
        TestArena arena;
        std::vector<game::InputCommand> commands(30);  // settle onto the floor
        // 60 ticks is long enough to reach top speed but short enough to stay
        // on the 40 m test floor (running off the edge would make the player
        // airborne, which disables sprint).
        for (int i = 0; i < 60; ++i) {
            game::InputCommand c;
            c.buttons = static_cast<std::uint16_t>(kForward | extra_buttons);
            commands.push_back(c);
        }
        const game::PlayerState state = simulate(arena, commands, {0.0f, 0.1f, 0.0f});
        return state;
    };

    const game::PlayerState walk = settle_and_run(0);
    const game::PlayerState sprint = settle_and_run(kSprint);
    const game::PlayerState crouch = settle_and_run(kCrouch);

    const auto speed = [](const game::PlayerState& s) {
        return std::hypot(s.velocity.x, s.velocity.z);
    };
    CHECK(speed(walk) == Approx(game::kMove.max_speed).margin(0.2f));
    CHECK(speed(sprint) ==
          Approx(game::kMove.max_speed * game::kMove.sprint_multiplier).margin(0.3f));
    CHECK(speed(crouch) ==
          Approx(game::kMove.max_speed * game::kMove.crouch_multiplier).margin(0.2f));
    CHECK(speed(sprint) > speed(walk));
    CHECK(speed(crouch) < speed(walk));
    CHECK(crouch.crouching);
    CHECK(sprint.sprinting);
    CHECK_FALSE(walk.crouching);
}

TEST_CASE("sprint requires being grounded and pushing forward", "[movement]") {
    TestArena arena;
    eng::CharacterController controller{arena.world, {0.0f, 0.1f, 0.0f}};
    game::PlayerState state;
    state.position = {0.0f, 0.1f, 0.0f};
    for (int i = 0; i < 30; ++i) {
        game::advance_player(state, {}, game::kTickSeconds, controller, arena.world);
    }
    REQUIRE(state.on_ground);

    // Sprint held but walking backwards: not sprinting.
    game::InputCommand backward;
    backward.buttons = static_cast<std::uint16_t>(static_cast<std::uint16_t>(game::Button::Back) |
                                                  static_cast<std::uint16_t>(game::Button::Sprint));
    game::advance_player(state, backward, game::kTickSeconds, controller, arena.world);
    CHECK_FALSE(state.sprinting);

    // Sprint + forward on the ground: sprinting.
    game::InputCommand forward;
    forward.buttons = static_cast<std::uint16_t>(static_cast<std::uint16_t>(game::Button::Forward) |
                                                 static_cast<std::uint16_t>(game::Button::Sprint));
    game::advance_player(state, forward, game::kTickSeconds, controller, arena.world);
    CHECK(state.sprinting);
}

TEST_CASE("a player cannot stand up under a low ceiling", "[movement]") {
    TestArena arena;
    // Slab 1.4 m above the floor: enough for a crouched player, not a
    // standing one.
    arena.world.add_static_box({0.0f, 1.55f, 0.0f}, {2.0f, 0.15f, 2.0f});
    arena.world.optimize();

    eng::CharacterController controller{arena.world, {0.0f, 0.1f, 0.0f}};
    game::PlayerState state;
    state.position = {0.0f, 0.1f, 0.0f};

    game::InputCommand crouch;
    game::set_button(crouch, game::Button::Crouch, true);
    for (int i = 0; i < 30; ++i) {
        game::advance_player(state, crouch, game::kTickSeconds, controller, arena.world);
    }
    REQUIRE(state.crouching);
    CHECK_FALSE(game::has_standing_headroom(state.position, arena.world));

    // Releasing crouch under the slab must NOT stand the player up.
    for (int i = 0; i < 30; ++i) {
        game::advance_player(state, {}, game::kTickSeconds, controller, arena.world);
    }
    CHECK(state.crouching);

    // Step out from under it and the player stands.
    state.position = {8.0f, 0.1f, 0.0f};
    CHECK(game::has_standing_headroom(state.position, arena.world));
    for (int i = 0; i < 5; ++i) {
        game::advance_player(state, {}, game::kTickSeconds, controller, arena.world);
    }
    CHECK_FALSE(state.crouching);
}

TEST_CASE("eye height follows the stance", "[movement]") {
    game::PlayerState standing;
    game::PlayerState crouched;
    crouched.crouching = true;
    CHECK(game::eye_height_for(standing) == Approx(game::kMove.eye_height));
    CHECK(game::eye_height_for(crouched) == Approx(game::kMove.crouch_eye_height));
    CHECK(game::eye_height_for(crouched) < game::eye_height_for(standing));
}

TEST_CASE("raycast hits the floor with an upward normal", "[physics]") {
    TestArena arena;
    const auto hit = arena.world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->distance == Approx(5.0f).margin(1e-3f));
    CHECK(hit->normal.y == Approx(1.0f).margin(1e-3f));
    CHECK_FALSE(arena.world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 20.0f).has_value());
}

}  // namespace
