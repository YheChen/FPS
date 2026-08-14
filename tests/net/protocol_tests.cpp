#include "game/shared/protocol.h"

#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

template <typename Message>
std::vector<std::uint8_t> encode(const Message& message) {
    eng::ByteWriter writer;
    game::write(writer, message);
    return {writer.data().begin(), writer.data().end()};
}

// The reason byte used to go nowhere: the client logged it as an integer and
// showed every refusal as "server rejected the connection". It now decides
// behaviour -- VersionMismatch means wait for the other half of the deploy and
// retry, the others mean stop -- so the value has to survive the wire intact.
TEST_CASE("ServerReject carries which refusal it was", "[protocol]") {
    for (const game::RejectReason reason :
         {game::RejectReason::VersionMismatch, game::RejectReason::ServerFull,
          game::RejectReason::BadName}) {
        const auto bytes = encode(game::ServerReject{reason});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        REQUIRE(game::read_message_type(r) == game::MessageType::ServerReject);
        const auto m = game::read_server_reject(r);
        REQUIRE(m.has_value());
        CHECK(m->reason == reason);
    }
}

TEST_CASE("every reject reason has a name a player could read", "[protocol]") {
    CHECK(std::string_view{game::reject_reason_name(game::RejectReason::VersionMismatch)} ==
          "version mismatch");
    CHECK(std::string_view{game::reject_reason_name(game::RejectReason::ServerFull)} ==
          "server full");
    CHECK(std::string_view{game::reject_reason_name(game::RejectReason::BadName)} == "bad name");
    // A value off the end of the enum must still produce a string: this is
    // reached from a decoded packet, and the alternative is falling off the
    // end of a function that returns a pointer someone then prints.
    CHECK(std::string_view{game::reject_reason_name(static_cast<game::RejectReason>(200))} ==
          "unknown");
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

TEST_CASE("combat messages round-trip", "[protocol]") {
    {
        // A shotgun blast: one event, several pellet rays.
        game::FireEventMsg fire;
        fire.shooter = 2;
        fire.slot = 1;
        fire.from = {1.0f, 1.6f, 3.0f};
        fire.rays.push_back({{5.0f, 1.2f, -8.0f}, game::kNoPlayer});
        fire.rays.push_back({{6.0f, 1.3f, -8.5f}, 3});
        const auto bytes = encode(fire);
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        REQUIRE(game::read_message_type(r) == game::MessageType::FireEvent);
        const auto m = game::read_fire_event(r);
        REQUIRE(m.has_value());
        CHECK(m->shooter == 2);
        CHECK(m->slot == 1);
        REQUIRE(m->rays.size() == 2);
        CHECK(m->rays[0].to.z == -8.0f);
        CHECK(m->rays[0].hit_player == game::kNoPlayer);
        CHECK(m->rays[1].hit_player == 3);
    }
    {
        const auto bytes = encode(game::PlayerDamagedMsg{1, 0, 75.0f, 25.0f});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        const auto m = game::read_player_damaged(r);
        REQUIRE(m.has_value());
        CHECK(m->health == 75.0f);
    }
    {
        const auto bytes = encode(game::PlayerDiedMsg{3, game::kNoPlayer});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        const auto m = game::read_player_died(r);
        REQUIRE(m.has_value());
        CHECK(m->killer == game::kNoPlayer);
    }
    {
        const auto bytes = encode(game::PlayerRespawnedMsg{4, {1, 2, 3}});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        CHECK(game::read_player_respawned(r)->position.y == 2.0f);
    }
    {
        const auto bytes = encode(game::ScoreUpdateMsg{1, 12, 7});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        const auto m = game::read_score_update(r);
        CHECK(m->kills == 12);
        CHECK(m->deaths == 7);
    }
    {
        const auto bytes = encode(game::MatchStateMsg{game::MatchPhase::Ended, 8});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        const auto m = game::read_match_state(r);
        CHECK(m->phase == game::MatchPhase::Ended);
        CHECK(m->seconds_remaining == 8);
    }
    {
        const auto bytes = encode(game::WeaponStatusMsg{17, true});
        eng::ByteReader r{{bytes.data(), bytes.size()}};
        game::read_message_type(r);
        const auto m = game::read_weapon_status(r);
        CHECK(m->ammo == 17);
        CHECK(m->reloading);
    }
}

TEST_CASE("hostile combat messages are rejected", "[protocol]") {
    // Shooter id out of range.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::FireEvent));
        w.u8(99);  // invalid shooter
        for (int i = 0; i < 6; ++i) {
            w.f32(0.0f);
        }
        w.u8(game::kNoPlayer);
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_fire_event(r) == std::nullopt);
    }
    // Negative health.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::PlayerDamaged));
        w.u8(0);
        w.u8(1);
        w.f32(-5.0f);
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_player_damaged(r) == std::nullopt);
    }
    // Invalid match phase.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::MatchState));
        w.u8(7);
        w.u16(100);
        eng::ByteReader r{w.data()};
        game::read_message_type(r);
        CHECK(game::read_match_state(r) == std::nullopt);
    }
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

TEST_CASE("the leaderboard round-trips and rejects hostile input", "[protocol]") {
    game::LeaderboardMsg sent;
    sent.entries.push_back({"alice", 12, 3, 2});
    sent.entries.push_back({"bob", 4, 9, 1});

    const auto bytes = encode(sent);
    eng::ByteReader r{{bytes.data(), bytes.size()}};
    REQUIRE(game::read_message_type(r) == game::MessageType::Leaderboard);
    const auto got = game::read_leaderboard(r);
    REQUIRE(got.has_value());
    REQUIRE(got->entries.size() == 2);
    CHECK(got->entries[0].name == "alice");
    CHECK(got->entries[0].kills == 12);
    CHECK(got->entries[0].deaths == 3);
    CHECK(got->entries[0].matches == 2);
    CHECK(got->entries[1].name == "bob");

    // The writer truncates rather than trusting its caller: the wire count is
    // a u8 and the reader enforces the same cap, so an over-long board must
    // not produce a message the reader will then reject.
    {
        game::LeaderboardMsg huge;
        for (std::size_t i = 0; i < game::kLeaderboardSize * 3; ++i) {
            huge.entries.push_back({"p" + std::to_string(i), 1, 1, 1});
        }
        const auto encoded = encode(huge);
        eng::ByteReader reader{{encoded.data(), encoded.size()}};
        game::read_message_type(reader);
        const auto parsed = game::read_leaderboard(reader);
        REQUIRE(parsed.has_value());
        CHECK(parsed->entries.size() == game::kLeaderboardSize);
    }

    // A declared count beyond the cap is an allocation request, not a message.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::Leaderboard));
        w.u8(255);
        eng::ByteReader reader{w.data()};
        game::read_message_type(reader);
        CHECK(game::read_leaderboard(reader) == std::nullopt);
    }

    // Truncated at every possible point.
    for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
        std::vector<std::uint8_t> partial{bytes.begin(),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(cut)};
        eng::ByteReader reader{{partial.data(), partial.size()}};
        game::read_message_type(reader);
        CHECK(game::read_leaderboard(reader) == std::nullopt);
    }

    // Trailing garbage.
    {
        auto extra = bytes;
        extra.push_back(0xFF);
        eng::ByteReader reader{{extra.data(), extra.size()}};
        game::read_message_type(reader);
        CHECK(game::read_leaderboard(reader) == std::nullopt);
    }
}

TEST_CASE("the killcam round-trips and rejects hostile input", "[protocol]") {
    game::KillCamMsg sent;
    sent.killer = 3;
    sent.samples.push_back({{1.0f, 2.0f, 3.0f}, 0.5f, -0.25f});
    sent.samples.push_back({{4.0f, 5.0f, 6.0f}, -1.5f, 0.75f});

    const auto bytes = encode(sent);
    eng::ByteReader r{{bytes.data(), bytes.size()}};
    REQUIRE(game::read_message_type(r) == game::MessageType::KillCam);
    const auto got = game::read_kill_cam(r);
    REQUIRE(got.has_value());
    CHECK(got->killer == 3);
    REQUIRE(got->samples.size() == 2);
    CHECK(got->samples[0].position.y == Catch::Approx(2.0f));
    CHECK(got->samples[0].yaw == Catch::Approx(0.5f));
    CHECK(got->samples[1].pitch == Catch::Approx(0.75f));

    // The writer truncates rather than trusting its caller; the count is a u8
    // and the reader enforces the same cap.
    {
        game::KillCamMsg huge;
        huge.killer = 1;
        for (std::size_t i = 0; i < game::kKillCamSamples * 2; ++i) {
            huge.samples.push_back({{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f});
        }
        const auto encoded = encode(huge);
        eng::ByteReader reader{{encoded.data(), encoded.size()}};
        game::read_message_type(reader);
        const auto parsed = game::read_kill_cam(reader);
        REQUIRE(parsed.has_value());
        CHECK(parsed->samples.size() == game::kKillCamSamples);
    }

    // A count past the cap is an allocation request, not a message.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::KillCam));
        w.u8(0);
        w.u8(255);
        eng::ByteReader reader{w.data()};
        game::read_message_type(reader);
        CHECK(game::read_kill_cam(reader) == std::nullopt);
    }

    // A NaN would not crash -- it would drive the camera transform to
    // nowhere and produce a black screen nobody could explain.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::KillCam));
        w.u8(0);
        w.u8(1);
        w.u32(0x7FC00000u);  // quiet NaN
        for (int i = 0; i < 4; ++i) {
            w.f32(0.0f);
        }
        eng::ByteReader reader{w.data()};
        game::read_message_type(reader);
        CHECK(game::read_kill_cam(reader) == std::nullopt);
    }

    // An out-of-range killer id, which would index a player array.
    {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(game::MessageType::KillCam));
        w.u8(200);  // neither a valid slot nor kNoPlayer
        w.u8(0);
        eng::ByteReader reader{w.data()};
        game::read_message_type(reader);
        CHECK(game::read_kill_cam(reader) == std::nullopt);
    }

    // Truncated at every offset, and trailing junk.
    for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
        std::vector<std::uint8_t> partial{bytes.begin(),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(cut)};
        eng::ByteReader reader{{partial.data(), partial.size()}};
        game::read_message_type(reader);
        CHECK(game::read_kill_cam(reader) == std::nullopt);
    }
    {
        auto extra = bytes;
        extra.push_back(0xFF);
        eng::ByteReader reader{{extra.data(), extra.size()}};
        game::read_message_type(reader);
        CHECK(game::read_kill_cam(reader) == std::nullopt);
    }
}

// --- WebRTC signalling (M19b) ----------------------------------------------
// These three are the only messages the game layer never sees; a signalling
// router in front of ServerGame consumes them. That makes their framing the
// only thing standing between a hostile browser and the SDP parser.

TEST_CASE("rtc offer and answer round-trip a realistic SDP", "[protocol]") {
    const std::string sdp =
        "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\n" + std::string(3000, 'a');

    eng::ByteWriter w;
    write(w, game::RtcOfferMsg{sdp});
    eng::ByteReader r{w.data()};
    REQUIRE(game::read_message_type(r) == game::MessageType::RtcOffer);
    const auto offer = game::read_rtc_offer(r);
    REQUIRE(offer);
    CHECK(offer->sdp == sdp);

    eng::ByteWriter w2;
    write(w2, game::RtcAnswerMsg{sdp});
    eng::ByteReader r2{w2.data()};
    REQUIRE(game::read_message_type(r2) == game::MessageType::RtcAnswer);
    const auto answer = game::read_rtc_answer(r2);
    REQUIRE(answer);
    CHECK(answer->sdp == sdp);
}

TEST_CASE("rtc candidate round-trips", "[protocol]") {
    eng::ByteWriter w;
    write(w, game::RtcCandidateMsg{"candidate:1 1 UDP 2130706431 192.168.1.2 54321 typ host", "0"});
    eng::ByteReader r{w.data()};
    REQUIRE(game::read_message_type(r) == game::MessageType::RtcCandidate);
    const auto candidate = game::read_rtc_candidate(r);
    REQUIRE(candidate);
    CHECK(candidate->candidate == "candidate:1 1 UDP 2130706431 192.168.1.2 54321 typ host");
    CHECK(candidate->mid == "0");
}

TEST_CASE("an oversized SDP is rejected rather than allocated", "[protocol]") {
    // Hand-framed: the writer would never produce this, but a peer can.
    eng::ByteWriter w;
    w.u8(static_cast<std::uint8_t>(game::MessageType::RtcOffer));
    w.u16(60000);  // claims 60 KB, sends none
    eng::ByteReader r{w.data()};
    REQUIRE(game::read_message_type(r) == game::MessageType::RtcOffer);
    CHECK_FALSE(game::read_rtc_offer(r).has_value());
}

TEST_CASE("signalling messages reject trailing bytes", "[protocol]") {
    eng::ByteWriter w;
    write(w, game::RtcAnswerMsg{"v=0"});
    std::vector<std::uint8_t> raw{w.data().begin(), w.data().end()};
    raw.push_back(0x00);  // one byte too many

    eng::ByteReader r{raw};
    REQUIRE(game::read_message_type(r) == game::MessageType::RtcAnswer);
    CHECK_FALSE(game::read_rtc_answer(r).has_value());
}

TEST_CASE("every message type is inside the accepted type range", "[protocol]") {
    // read_message_type bounds-checks against the LAST enumerator, so adding
    // a message without moving that bound silently makes it unreadable.
    //
    // THIS TEST IS SUPPOSED TO FAIL when a message is added and the bound is
    // not moved with it -- and it did exactly that when M50 added ChatSend
    // past what was then the end. Update kLastMessageType below along with
    // the enum; that is the point of it being written down twice.
    constexpr auto kLastMessageType = game::MessageType::ChatMessage;

    for (const auto type :
         {game::MessageType::ClientHello, game::MessageType::RtcOffer, game::MessageType::RtcAnswer,
          game::MessageType::RtcCandidate, game::MessageType::ChatSend, kLastMessageType}) {
        eng::ByteWriter w;
        w.u8(static_cast<std::uint8_t>(type));
        eng::ByteReader r{w.data()};
        CHECK(game::read_message_type(r) == type);
    }
    // ...and one past the end is still refused.
    eng::ByteWriter w;
    w.u8(static_cast<std::uint8_t>(kLastMessageType) + 1);
    eng::ByteReader r{w.data()};
    CHECK_FALSE(game::read_message_type(r).has_value());
}

// --- text chat (M50) --------------------------------------------------------
// ChatSend is the only message whose PAYLOAD a player composes, so it is the
// only one where the content, not just the framing, is hostile input.

TEST_CASE("chat messages round-trip", "[protocol][chat]") {
    eng::ByteWriter w;
    write(w, game::ChatMessageMsg{3, "nice shot"});
    eng::ByteReader r{w.data()};
    REQUIRE(game::read_message_type(r) == game::MessageType::ChatMessage);
    const auto message = game::read_chat_message(r);
    REQUIRE(message);
    CHECK(message->sender == 3);
    CHECK(message->text == "nice shot");
}

TEST_CASE("a chat message claiming an impossible sender is rejected", "[protocol][chat]") {
    eng::ByteWriter w;
    w.u8(static_cast<std::uint8_t>(game::MessageType::ChatMessage));
    w.u8(200);  // no such player
    w.str("hello");
    eng::ByteReader r{w.data()};
    REQUIRE(game::read_message_type(r) == game::MessageType::ChatMessage);
    CHECK_FALSE(game::read_chat_message(r).has_value());
}

TEST_CASE("sanitize_chat strips control characters", "[protocol][chat]") {
    // A newline would let one message forge a second chat line -- and a
    // second line in the server's log, which is where this gets dangerous.
    CHECK(game::sanitize_chat("hello\nadmin: banned") == "helloadmin: banned");
    CHECK(game::sanitize_chat("a\rb") == "ab");
    CHECK(game::sanitize_chat("a\tb") == "ab");
    CHECK(game::sanitize_chat("\x1b[31mred") == "[31mred");  // ESC removed, text kept
    CHECK(game::sanitize_chat(std::string("nul\0here", 8)) == "nulhere");
}

TEST_CASE("sanitize_chat trims and rejects the empty", "[protocol][chat]") {
    CHECK(game::sanitize_chat("  spaced  ") == "spaced");
    CHECK(game::sanitize_chat("").empty());
    CHECK(game::sanitize_chat("      ").empty());
    CHECK(game::sanitize_chat("\n\n\n").empty());
}

TEST_CASE("sanitize_chat caps length without splitting a character", "[protocol][chat]") {
    const std::string plain(300, 'x');
    CHECK(game::sanitize_chat(plain).size() == game::kMaxChatLength);

    // 'é' is two bytes. Filling to exactly one byte past the cap must drop
    // the whole character rather than leave half of it: a lone continuation
    // byte is invalid UTF-8, and the renderer would be handed it purely
    // because of where the cap happened to fall.
    std::string accented;
    while (accented.size() < game::kMaxChatLength + 4) {
        accented += "\xC3\xA9";
    }
    const std::string clean = game::sanitize_chat(accented);
    CHECK(clean.size() <= game::kMaxChatLength);
    CHECK(clean.size() % 2 == 0);  // only whole two-byte characters survived
    // ...and nothing left dangling at either end.
    CHECK((static_cast<unsigned char>(clean.back()) & 0xC0) == 0x80);
}

TEST_CASE("chat keeps text a player would legitimately type", "[protocol][chat]") {
    CHECK(game::sanitize_chat("gg wp :)") == "gg wp :)");
    CHECK(game::sanitize_chat("caf\xC3\xA9") == "caf\xC3\xA9");
    CHECK(game::sanitize_chat("<b>not html</b>") == "<b>not html</b>");
}

}  // namespace
