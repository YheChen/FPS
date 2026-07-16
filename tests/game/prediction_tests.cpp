#include "game/shared/prediction.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "game/shared/player_movement.h"

namespace {

using Catch::Approx;

struct TestArena {
    eng::PhysicsWorld world;
    TestArena() {
        world.add_static_box({0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
        world.optimize();
    }
};

game::InputCommand forward_command(std::uint32_t seq) {
    game::InputCommand c;
    c.sequence = seq;
    game::set_button(c, game::Button::Forward, true);
    return c;
}

// An authoritative "server" running the same shared movement code.
struct FakeServer {
    eng::CharacterController controller;
    game::PlayerState state;
    std::uint32_t last_processed = 0;

    FakeServer(eng::PhysicsWorld& world, const glm::vec3& spawn) : controller(world, spawn) {
        state.position = spawn;
    }

    void process(eng::PhysicsWorld& world, const game::InputCommand& command) {
        game::advance_player(state, command, game::kTickSeconds, controller, world);
        last_processed = command.sequence;
    }
};

TEST_CASE("prediction matches the server exactly on a clean network", "[prediction]") {
    TestArena arena;
    const glm::vec3 spawn{0.0f, 0.1f, 0.0f};
    game::Prediction prediction{arena.world, spawn};
    FakeServer server{arena.world, spawn};

    // Client predicts and the server processes the same inputs with a
    // 5-tick delay; every reconcile must produce ~zero error.
    std::vector<game::InputCommand> sent;
    float max_error = 0.0f;
    for (std::uint32_t seq = 1; seq <= 120; ++seq) {
        const auto command = forward_command(seq);
        prediction.tick(command);
        sent.push_back(command);

        if (seq >= 5) {
            server.process(arena.world, sent[seq - 5]);
            const auto result = prediction.reconcile(server.state.position, server.state.velocity,
                                                     server.state.on_ground, server.last_processed);
            max_error = std::max(max_error, result.error_meters);
        }
    }
    CHECK(max_error == Approx(0.0f).margin(1e-5f));
    CHECK(prediction.pending().size() == 4);  // the in-flight tail
}

TEST_CASE("reconciliation converges after a forced server correction", "[prediction]") {
    TestArena arena;
    const glm::vec3 spawn{0.0f, 0.1f, 0.0f};
    game::Prediction prediction{arena.world, spawn};
    FakeServer server{arena.world, spawn};

    for (std::uint32_t seq = 1; seq <= 30; ++seq) {
        const auto command = forward_command(seq);
        prediction.tick(command);
        server.process(arena.world, command);
    }

    // The server disagrees (e.g. it rejected some movement): teleport its
    // state sideways, then reconcile.
    server.state.position.x += 0.8f;
    auto result = prediction.reconcile(server.state.position, server.state.velocity,
                                       server.state.on_ground, server.last_processed);
    CHECK(result.corrected);
    CHECK(result.error_meters == Approx(0.8f).margin(0.05f));
    // Prediction adopted the server state (no pending inputs remained).
    CHECK(prediction.state().position.x == Approx(server.state.position.x).margin(1e-4f));
    // The visual offset hides the correction, then decays away.
    CHECK(glm::length(prediction.smoothing_offset()) == Approx(0.8f).margin(0.05f));
    for (int i = 0; i < 120; ++i) {
        prediction.update_smoothing(game::kTickSeconds);
    }
    CHECK(glm::length(prediction.smoothing_offset()) < 1e-3f);
}

TEST_CASE("huge corrections snap instead of smoothing", "[prediction]") {
    TestArena arena;
    game::Prediction prediction{arena.world, {0.0f, 0.1f, 0.0f}};
    prediction.tick(forward_command(1));

    const auto result = prediction.reconcile({10.0f, 0.1f, 10.0f}, {0.0f, 0.0f, 0.0f}, true, 1);
    CHECK(result.corrected);
    CHECK(result.error_meters > game::Prediction::kSnapThresholdMeters);
    CHECK(glm::length(prediction.smoothing_offset()) == 0.0f);
}

TEST_CASE("acked inputs are dropped from the pending queue", "[prediction]") {
    TestArena arena;
    game::Prediction prediction{arena.world, {0.0f, 0.1f, 0.0f}};
    for (std::uint32_t seq = 1; seq <= 10; ++seq) {
        prediction.tick(forward_command(seq));
    }
    CHECK(prediction.pending().size() == 10);

    const auto state = prediction.state();
    prediction.reconcile(state.position, state.velocity, state.on_ground, 7);
    CHECK(prediction.pending().size() == 3);
    CHECK(prediction.pending().front().sequence == 8);
}

}  // namespace
