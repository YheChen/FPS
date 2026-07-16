#include "game/shared/protocol.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

template <typename Message>
std::vector<std::uint8_t> encode(const Message& message) {
    eng::ByteWriter writer;
    game::write(writer, message);
    return {writer.data().begin(), writer.data().end()};
}

TEST_CASE("hello/welcome/joined/left round-trip", "[protocol]") {
    {
        const auto bytes = encode(game::ClientHello{"alice"});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        REQUIRE(game::read_message_type(r) == game::MessageType::ClientHello);
        const auto m = game::read_client_hello(r);
        REQUIRE(m.has_value());
        CHECK(m->name == "alice");
    }
    {
        game::ServerWelcome welcome;
        welcome.player_id = 3;
        welcome.server_tick = 12345;
        welcome.map = "maps/arena01.glb";
        const auto bytes = encode(welcome);
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        REQUIRE(game::read_message_type(r) == game::MessageType::ServerWelcome);
        const auto m = game::read_server_welcome(r);
        REQUIRE(m.has_value());
        CHECK(m->player_id == 3);
        CHECK(m->server_tick == 12345);
        CHECK(m->map == "maps/arena01.glb");
        CHECK(m->tick_rate == 60);
        CHECK(m->snapshot_rate == 20);
    }
    {
        const auto bytes = encode(game::PlayerJoined{5, "bob"});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        REQUIRE(game::read_message_type(r) == game::MessageType::PlayerJoined);
        const auto m = game::read_player_joined(r);
        REQUIRE(m.has_value());
        CHECK(m->player_id == 5);
        CHECK(m->name == "bob");
    }
    {
        const auto bytes = encode(game::PlayerLeft{2});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        REQUIRE(game::read_message_type(r) == game::MessageType::PlayerLeft);
        CHECK(game::read_player_left(r)->player_id == 2);
    }
}

TEST_CASE("input packet round-trips with reconstructed sequences", "[protocol]") {
    game::InputPacket packet;
    packet.newest_sequence = 100;
    packet.client_tick = 555;
    for (int i = 0; i < 3; ++i) {
        game::InputCommand c;
        c.yaw = 0.5f + 0.1f * static_cast<float>(i);
        c.pitch = -0.3f;
        c.buttons = static_cast<std::uint8_t>(i + 1);
        packet.commands.push_back(c);
    }
    const auto bytes = encode(packet);
    eng::ByteReader r{{bytes.data(), bytes.size()}};
    REQUIRE(game::read_message_type(r) == game::MessageType::Input);
    const auto m = game::read_input_packet(r);
    REQUIRE(m.has_value());
    CHECK(m->newest_sequence == 100);
    CHECK(m->client_tick == 555);
    REQUIRE(m->commands.size() == 3);
    CHECK(m->commands[0].sequence == 98);  // oldest first
    CHECK(m->commands[1].sequence == 99);
    CHECK(m->commands[2].sequence == 100);
    CHECK(m->commands[2].buttons == 3);
    CHECK(m->commands[0].yaw == Catch::Approx(0.5f));
}

TEST_CASE("snapshot round-trips", "[protocol]") {
    game::Snapshot snapshot;
    snapshot.server_tick = 999;
    snapshot.last_processed_input = 42;
    game::SnapshotPlayer p;
    p.player_id = 1;
    p.position = {1.0f, 2.0f, 3.0f};
    p.velocity = {-1.0f, 0.0f, 4.0f};
    p.yaw = 0.7f;
    p.pitch = -0.2f;
    p.flags = 1;
    snapshot.players.push_back(p);

    const auto bytes = encode(snapshot);
    eng::ByteReader r{{bytes.data(), bytes.size()}};
    REQUIRE(game::read_message_type(r) == game::MessageType::Snapshot);
    const auto m = game::read_snapshot(r);
    REQUIRE(m.has_value());
    CHECK(m->server_tick == 999);
    CHECK(m->last_processed_input == 42);
    REQUIRE(m->players.size() == 1);
    CHECK(m->players[0].position.y == 2.0f);
    CHECK(m->players[0].flags == 1);
}

TEST_CASE("hostile packets are rejected", "[protocol]") {
    // Unknown message type.
    {
        const std::uint8_t bytes[] = {200};
        eng::ByteReader r{{bytes, 1}};
        CHECK(game::read_message_type(r) == std::nullopt);
    }
    // Wrong protocol version in hello.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::ClientHello));
        w.u16(game::kProtocolVersion + 1);
        w.str("x");
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_client_hello(r) == std::nullopt);
    }
    // Name too long.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::ClientHello));
        w.u16(game::kProtocolVersion);
        w.str("this-name-is-way-too-long-for-us");
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_client_hello(r) == std::nullopt);
    }
    // Input with too many commands.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::Input));
        w.u32(10);
        w.u32(1);
        w.u8(9);  // count 9 > redundancy 3
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_input_packet(r) == std::nullopt);
    }
    // Input with an insane pitch.
    {
        game::InputPacket packet;
        packet.newest_sequence = 1;
        game::InputCommand c;
        c.pitch = 3.0f;  // ~172 degrees: impossible
        packet.commands.push_back(c);
        const auto bytes = encode(packet);
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        CHECK(game::read_input_packet(r) == std::nullopt);
    }
    // Truncated snapshot.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::Snapshot));
        w.u32(5);
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_snapshot(r) == std::nullopt);
    }
    // Trailing garbage after a valid message.
    {
        auto bytes = encode(game::PlayerLeft{1});
        bytes.push_back(0xFF);
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        CHECK(game::read_player_left(r) == std::nullopt);
    }
}

}  // namespace
