#include "game/shared/replay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"
#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
#include "game/shared/player_movement.h"

namespace {

using Catch::Approx;

game::InputCommand make_command(std::uint32_t sequence, float yaw, std::uint16_t buttons,
                                std::uint8_t slot = 0) {
    game::InputCommand command;
    command.sequence = sequence;
    command.yaw = yaw;
    command.pitch = 0.1f;
    command.buttons = buttons;
    command.weapon_slot = slot;
    return command;
}

game::Replay small_replay() {
    game::Replay replay;
    replay.map_path = "maps/arena01.glb";
    replay.tick_rate_hz = 60;
    replay.players = {
        {0, "alice", {1.0f, 2.0f, 3.0f}},
        {3, "bob", {-4.0f, 0.5f, 7.5f}},
    };
    replay.frames = {
        {10, {{0, make_command(1, 0.5f, 0x0001)}, {3, make_command(9, -1.25f, 0x0020, 2)}}},
        {11, {{0, make_command(2, 0.6f, 0x0011, 1)}}},
        {14, {{3, make_command(10, 2.0f, 0x0000, 3)}}},
    };
    return replay;
}

TEST_CASE("a replay round-trips through encode and decode", "[replay]") {
    const game::Replay original = small_replay();
    const auto decoded = game::decode_replay(game::encode_replay(original));
    REQUIRE(decoded.has_value());

    CHECK(decoded->map_path == original.map_path);
    CHECK(decoded->tick_rate_hz == original.tick_rate_hz);

    REQUIRE(decoded->players.size() == original.players.size());
    for (std::size_t i = 0; i < original.players.size(); ++i) {
        CHECK(decoded->players[i].id == original.players[i].id);
        CHECK(decoded->players[i].name == original.players[i].name);
        CHECK(decoded->players[i].spawn.x == Approx(original.players[i].spawn.x));
        CHECK(decoded->players[i].spawn.z == Approx(original.players[i].spawn.z));
    }

    REQUIRE(decoded->frames.size() == original.frames.size());
    for (std::size_t f = 0; f < original.frames.size(); ++f) {
        CHECK(decoded->frames[f].tick == original.frames[f].tick);
        REQUIRE(decoded->frames[f].commands.size() == original.frames[f].commands.size());
        for (std::size_t c = 0; c < original.frames[f].commands.size(); ++c) {
            const game::ReplayCommand& a = original.frames[f].commands[c];
            const game::ReplayCommand& b = decoded->frames[f].commands[c];
            CHECK(b.player_id == a.player_id);
            CHECK(b.command.sequence == a.command.sequence);
            CHECK(b.command.yaw == Approx(a.command.yaw));
            CHECK(b.command.pitch == Approx(a.command.pitch));
            CHECK(b.command.buttons == a.command.buttons);
            CHECK(b.command.weapon_slot == a.command.weapon_slot);
        }
    }

    CHECK(decoded->find_player(3) != nullptr);
    CHECK(decoded->find_player(3)->name == "bob");
    CHECK(decoded->find_player(9) == nullptr);
}

TEST_CASE("a truncated replay is rejected at every length", "[replay]") {
    const std::vector<std::uint8_t> bytes = game::encode_replay(small_replay());
    REQUIRE(bytes.size() > 8);

    // Every proper prefix must fail. A decoder that accepts one of these is
    // reading past the buffer or inventing data.
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const std::span<const std::uint8_t> prefix{bytes.data(), length};
        INFO("prefix length " << length);
        CHECK_FALSE(game::decode_replay(prefix).has_value());
    }
    CHECK(game::decode_replay(bytes).has_value());
}

TEST_CASE("a replay with trailing bytes is rejected", "[replay]") {
    std::vector<std::uint8_t> bytes = game::encode_replay(small_replay());
    bytes.push_back(0x42);
    CHECK_FALSE(game::decode_replay(bytes).has_value());
}

TEST_CASE("bad magic and bad version are rejected", "[replay]") {
    std::vector<std::uint8_t> bytes = game::encode_replay(small_replay());
    std::vector<std::uint8_t> wrong_magic = bytes;
    wrong_magic[0] = 'X';
    CHECK_FALSE(game::decode_replay(wrong_magic).has_value());

    // Version sits right after the 4 magic bytes, little-endian u16.
    std::vector<std::uint8_t> wrong_version = bytes;
    wrong_version[4] = static_cast<std::uint8_t>(game::kReplayVersion + 1);
    CHECK_FALSE(game::decode_replay(wrong_version).has_value());

    CHECK(game::decode_replay(std::span<const std::uint8_t>{}).has_value() == false);
}

TEST_CASE("out-of-range fields in a replay are rejected", "[replay]") {
    // A replay file is as untrusted as a packet: a hand-edited weapon slot or
    // player id must not reach the simulation.
    game::Replay bad_slot = small_replay();
    bad_slot.frames[0].commands[0].command.weapon_slot = 99;
    CHECK_FALSE(game::decode_replay(game::encode_replay(bad_slot)).has_value());

    game::Replay bad_player = small_replay();
    bad_player.frames[0].commands[0].player_id = 200;
    CHECK_FALSE(game::decode_replay(game::encode_replay(bad_player)).has_value());

    game::Replay bad_spawn_id = small_replay();
    bad_spawn_id.players[0].id = 200;
    CHECK_FALSE(game::decode_replay(game::encode_replay(bad_spawn_id)).has_value());
}

TEST_CASE("frames whose ticks do not advance are rejected", "[replay]") {
    game::Replay backwards = small_replay();
    backwards.frames[2].tick = 5;  // earlier than frame 1
    CHECK_FALSE(game::decode_replay(game::encode_replay(backwards)).has_value());

    game::Replay repeated = small_replay();
    repeated.frames[1].tick = repeated.frames[0].tick;
    CHECK_FALSE(game::decode_replay(game::encode_replay(repeated)).has_value());
}

TEST_CASE("the recorder groups commands by tick and ignores misuse", "[replay]") {
    game::ReplayRecorder recorder;
    // Inert before begin(), so the server can hold one unconditionally.
    recorder.record(1, 0, make_command(1, 0.0f, 0));
    CHECK_FALSE(recorder.recording());
    CHECK(recorder.frame_count() == 0);

    recorder.begin("maps/arena01.glb", 60);
    CHECK(recorder.recording());
    recorder.add_player(0, "alice", {1.0f, 0.0f, 0.0f});
    recorder.add_player(1, "bob", {2.0f, 0.0f, 0.0f});
    // A repeated id updates rather than duplicating.
    recorder.add_player(1, "bobby", {3.0f, 0.0f, 0.0f});
    REQUIRE(recorder.replay().players.size() == 2);
    CHECK(recorder.replay().players[1].name == "bobby");
    CHECK(recorder.replay().players[1].spawn.x == Approx(3.0f));

    recorder.record(7, 0, make_command(1, 0.0f, 0));
    recorder.record(7, 1, make_command(1, 0.0f, 0));
    recorder.record(8, 0, make_command(2, 0.0f, 0));
    REQUIRE(recorder.frame_count() == 2);
    CHECK(recorder.replay().frames[0].commands.size() == 2);
    CHECK(recorder.replay().frames[1].commands.size() == 1);

    // Out-of-order ticks are dropped: replaying them would not reproduce what
    // the server actually ran.
    recorder.record(6, 0, make_command(3, 0.0f, 0));
    CHECK(recorder.frame_count() == 2);
    CHECK(recorder.replay().frames[1].commands.size() == 1);

    // Out-of-range player ids never enter the recording.
    recorder.record(9, 250, make_command(4, 0.0f, 0));
    CHECK(recorder.frame_count() == 2);
}

TEST_CASE("a recorded replay survives a file round trip", "[replay]") {
    const auto path = std::filesystem::temp_directory_path() / "fps_replay_test.fpsr";
    std::filesystem::remove(path);

    const game::Replay original = small_replay();
    REQUIRE(game::write_replay_file(path, original));

    const auto loaded = game::read_replay_file(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->frames.size() == original.frames.size());
    CHECK(loaded->players.size() == original.players.size());
    std::filesystem::remove(path);

    CHECK_FALSE(game::read_replay_file(path).has_value());  // now missing
}

// --- the point of the whole milestone --------------------------------------

// Runs `commands` through the shared movement code and returns the final
// state. Used twice below: once live while recording, once from the replay.
game::PlayerState simulate(eng::PhysicsWorld& world, const glm::vec3& spawn,
                           const std::vector<game::InputCommand>& commands) {
    eng::CharacterController controller{world, spawn};
    game::PlayerState player;
    player.position = spawn;
    for (const game::InputCommand& command : commands) {
        game::advance_player(player, command, game::kTickSeconds, controller, world);
    }
    return player;
}

TEST_CASE("replaying recorded inputs reproduces the simulation exactly", "[replay]") {
    // This is the test that makes the format worth having. Inputs are the
    // only thing recorded, so if the replayed run diverges by even one bit,
    // the simulation is not deterministic -- and prediction, reconciliation
    // and lag compensation all rest on it being deterministic.
    const auto assets_root = eng::find_assets_root();
    REQUIRE(assets_root.has_value());
    eng::AssetCache assets{*assets_root, /*decode_images=*/false};
    const eng::GltfModel* map = assets.model("maps/arena01.glb");
    REQUIRE(map != nullptr);

    eng::PhysicsWorld world;
    for (const eng::GltfNode& node : map->nodes) {
        if (node.mesh < 0) {
            continue;
        }
        for (const eng::GltfPrimitive& primitive :
             map->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
            world.add_static_mesh(primitive.mesh, node.transform);
        }
    }
    world.optimize();

    const glm::vec3 spawn{15.0f, 0.5f, 15.0f};

    // A varied input stream: turning, strafing, sprinting, jumping and
    // crouching, so the run exercises ground contact, air control and stance
    // changes rather than walking in a straight line.
    std::vector<game::InputCommand> commands;
    game::ReplayRecorder recorder;
    recorder.begin("maps/arena01.glb", 60);
    recorder.add_player(0, "recorder", spawn);

    for (std::uint32_t tick = 0; tick < 240; ++tick) {
        game::InputCommand command;
        command.sequence = tick;
        command.yaw = static_cast<float>(tick) * 0.017f;
        command.pitch = 0.15f * std::sin(static_cast<float>(tick) * 0.05f);
        game::set_button(command, game::Button::Forward, true);
        game::set_button(command, game::Button::Left, (tick / 20) % 2 == 0);
        game::set_button(command, game::Button::Sprint, (tick / 37) % 2 == 0);
        game::set_button(command, game::Button::Jump, tick % 41 == 0);
        game::set_button(command, game::Button::Crouch, (tick / 53) % 3 == 0);
        commands.push_back(command);
        recorder.record(tick, 0, command);
    }

    const game::PlayerState live = simulate(world, spawn, commands);

    // Go through the actual file format, not just the in-memory recorder, so
    // a serialization bug cannot hide behind an in-process shortcut.
    const auto decoded = game::decode_replay(game::encode_replay(recorder.replay()));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->frames.size() == commands.size());

    std::vector<game::InputCommand> replayed_commands;
    replayed_commands.reserve(decoded->frames.size());
    for (const game::ReplayFrame& frame : decoded->frames) {
        REQUIRE(frame.commands.size() == 1);
        replayed_commands.push_back(frame.commands[0].command);
    }
    const game::PlayerState replayed = simulate(world, spawn, replayed_commands);

    // Bit-for-bit, not approximately. Approximate equality here would let a
    // real determinism drift slip through, which is the one thing this test
    // exists to catch.
    CHECK(std::memcmp(&live.position, &replayed.position, sizeof(glm::vec3)) == 0);
    CHECK(std::memcmp(&live.velocity, &replayed.velocity, sizeof(glm::vec3)) == 0);
    CHECK(live.on_ground == replayed.on_ground);
    CHECK(live.crouching == replayed.crouching);
    CHECK(live.sprinting == replayed.sprinting);

    // And the run actually went somewhere, so the comparison is not trivially
    // true because nothing moved.
    CHECK(glm::length(live.position - spawn) > 3.0f);
}

}  // namespace
