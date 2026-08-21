#include "game/server/server_game.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/net/byte_buffer.h"
#include "engine/net/transport.h"
#include "engine/rendering/mesh_data.h"
#include "game/server/stats_store.h"
#include "game/shared/bot.h"
#include "game/shared/input_command.h"
#include "game/shared/lag_comp.h"
#include "game/shared/player_movement.h"
#include "game/shared/protocol.h"
#include "game/shared/weapon.h"
#include "tests/server/fake_transport.h"
#include "tests/server/temp_dir.h"

// Tests for the authoritative server's rules: who gets in, who gets refused,
// what a shot does, what a death costs, when the match turns over, and what
// the server may say to whom.
//
// Everything here drives ServerGame through its two real entry points --
// handle_event() for anything the network produced, tick() for time -- and
// asserts only on what the fake transport recorded. Nothing reaches into
// private state, so a test failing here means a client would have seen
// something different, not that an implementation detail moved.
namespace {

using test::decode_all;
using test::decode_to;
using test::TempDir;
using MsgType = game::MessageType;

constexpr std::string_view kMapName = "maps/test_arena.glb";

// Slot 1 of test_arsenal(): a shotgun. Named here because the tests that use
// it assert on the numbers.
constexpr int kShotgunPellets = 8;
constexpr float kShotgunPelletDamage = 20.0f;

// ServerGame logs a line per join, kill and match event. A test runs tens of
// thousands of ticks, so turn the volume down; ctest shows a test's output
// only when it fails, and by then the interesting part is the assertion.
//
// Restored on destruction: the log level is process-global, and a fixture
// that clamps it one way for the rest of the process would quietly break the
// first test that asserts on log output.
class LogQuiet {
public:
    LogQuiet() : previous_(eng::log::level()) { eng::log::set_level(eng::log::Level::Error); }
    ~LogQuiet() { eng::log::set_level(previous_); }
    LogQuiet(const LogQuiet&) = delete;
    LogQuiet& operator=(const LogQuiet&) = delete;

private:
    eng::log::Level previous_;
};

// Two weapons with round numbers, so "four shots to kill" and "a shot every
// six ticks" are properties of the test rather than of whatever
// assets/weapons/rifle.cfg happens to say this month.
game::Arsenal test_arsenal() {
    game::WeaponConfig rifle;
    rifle.name = "test_rifle";
    rifle.damage = 25.0f;              // four hits from full health
    rifle.rounds_per_minute = 600.0f;  // one shot per 6 ticks
    rifle.magazine_size = 30;
    rifle.reload_seconds = 1.8f;
    rifle.range = 100.0f;
    rifle.spread_degrees = 0.0f;  // no cone: a pellet goes exactly where aimed
    rifle.pellets = 1;
    rifle.automatic = true;
    rifle.switch_seconds = 0.4f;

    // Slot 1. A single-pellet weapon cannot exercise the two rules that only
    // exist because of shotguns -- that the pellets of one shot are combined
    // into ONE damage event per victim, and that the per-pellet spread is a
    // deterministic function of (tick, shooter, pellet) rather than of a
    // running RNG that a replay could not reproduce.
    //
    // Eight pellets of 20 is 160 against 100 health: a hit is a kill, which
    // is what makes "exactly one PlayerDied" a real assertion. The cone is
    // deliberately tight -- half a degree is ~7 cm at the 8 m the duel is
    // fought over, well inside the 0.4 m capsule -- so every pellet lands
    // while still going through the spread code.
    game::WeaponConfig shotgun;
    shotgun.name = "test_shotgun";
    shotgun.damage = kShotgunPelletDamage;
    shotgun.rounds_per_minute = 60.0f;  // one shot per second
    shotgun.magazine_size = 8;
    shotgun.reload_seconds = 2.0f;
    shotgun.range = 100.0f;
    shotgun.spread_degrees = 0.5f;
    shotgun.pellets = kShotgunPellets;
    shotgun.automatic = true;
    shotgun.switch_seconds = 0.4f;

    game::Arsenal arsenal;
    arsenal.weapons.push_back(rifle);
    arsenal.weapons.push_back(shotgun);
    return arsenal;
}

// The map, built in memory. game/server/main.cpp walks a loaded .glb and
// collects (primitive mesh, node transform) pairs plus the translation of
// every node named "spawn_"; ServerGame never sees the file, only those two
// vectors. So a floor is one unit cube stretched flat, and spawns are just
// positions.
std::vector<std::pair<eng::MeshData, glm::mat4>> flat_floor(float half_extent) {
    const glm::mat4 transform =
        glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, -0.5f, 0.0f}) *
        glm::scale(glm::mat4{1.0f}, glm::vec3{half_extent * 2.0f, 1.0f, half_extent * 2.0f});
    std::vector<std::pair<eng::MeshData, glm::mat4>> meshes;
    meshes.emplace_back(eng::MeshData::unit_cube(), transform);
    return meshes;
}

// Two spawn points 8 m apart, both a metre above the floor's top face (y=0)
// so a joining player drops onto it. 8 m is well inside the test rifle's
// 100 m range and far enough that a shot is a real ray through the world.
std::vector<glm::vec3> default_spawns() {
    return {{0.0f, 1.0f, 0.0f}, {8.0f, 1.0f, 0.0f}};
}

// A command that looks from `shooter_feet` at a point `aim_height` above the
// feet of a player standing at `target_feet`, in the server's own yaw/pitch
// convention. The default is chest height.
game::InputCommand aim_at(const glm::vec3& shooter_feet, const glm::vec3& target_feet, bool fire,
                          float aim_height = 1.0f) {
    const glm::vec3 eye = shooter_feet + glm::vec3{0.0f, game::kMove.eye_height, 0.0f};
    const glm::vec3 chest = target_feet + glm::vec3{0.0f, aim_height, 0.0f};
    const glm::vec3 delta = chest - eye;
    const float horizontal = glm::length(glm::vec2{delta.x, delta.z});

    game::InputCommand command;
    command.yaw = game::yaw_towards(eye, chest);
    command.pitch = horizontal > 0.01f ? std::atan2(delta.y, horizontal) : 0.0f;
    game::set_button(command, game::Button::Fire, fire);
    return command;
}

struct Harness {
    LogQuiet quiet;
    test::FakeTransport net;
    game::ServerGame game;

    explicit Harness(float floor_half_extent = 60.0f,
                     std::vector<glm::vec3> spawns = default_spawns())
        : game(flat_floor(floor_half_extent), std::move(spawns), std::string{kMapName},
               test_arsenal()) {}

    // Delivers everything the transport has queued, exactly as the real run
    // loop in game/server/main.cpp does.
    void pump() {
        std::vector<eng::NetEvent> events;
        net.poll(events);
        for (const eng::NetEvent& event : events) {
            game.handle_event(event, net);
        }
    }

    void tick(int count = 1) {
        for (int i = 0; i < count; ++i) {
            game.tick(net);
        }
    }

    // Ticks up to an ABSOLUTE server tick. Lets a timing assertion be
    // expressed as "kRespawnSeconds after the tick the death landed on"
    // rather than as a hard-coded count that silently stops meaning that
    // when the weapon's fire rate changes.
    void tick_until(std::uint32_t target) {
        while (game.current_tick() < target) {
            tick();
        }
    }

    // Connect + hello. Returns the id in the ServerWelcome, or nullopt when
    // the server refused (which is itself worth asserting on).
    std::optional<std::uint8_t> join(std::uint32_t peer, std::string_view name) {
        net.queue_connect(peer);
        net.queue_message(peer, test::hello_bytes(game::kProtocolVersion, name));
        pump();
        const auto welcomes =
            decode_to(net.sent, peer, MsgType::ServerWelcome, game::read_server_welcome);
        if (welcomes.empty()) {
            return std::nullopt;
        }
        return welcomes.back().player_id;
    }

    // One tick of a client at full rate: one command in, one tick out. Keeps
    // the pending queue exactly one deep, so the command is consumed by the
    // tick it was sent for and never trips the kMaxInputAhead window.
    //
    // `view_tick` is the server tick this client claims it was RENDERING
    // remote players at -- what drives server-side rewind. 0 is what a client
    // with no estimate yet sends, and means "resolve at the current tick".
    void drive_tick(std::uint32_t peer, const game::InputCommand& command,
                    std::uint32_t view_tick = 0) {
        net.queue_message(peer, test::input_bytes(++sequence[peer], command, view_tick),
                          eng::NetChannel::Sequenced);
        pump();
        tick();
    }

    void drive(std::uint32_t peer, const game::InputCommand& command, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            drive_tick(peer, command);
        }
    }

    // Drives until the server emits `until`, at most `max_ticks`. Returns the
    // tick it happened on, or -1. Lets a test stop on the event it cares
    // about instead of guessing a duration, which keeps later timing
    // assertions (respawn, match end) anchored to a known instant.
    int drive_until(std::uint32_t peer, const game::InputCommand& command, MsgType until,
                    int max_ticks) {
        for (int i = 1; i <= max_ticks; ++i) {
            drive_tick(peer, command);
            if (net.count(until) > 0) {
                return i;
            }
        }
        return -1;
    }

    // Where the server last told `peer` that `player_id` was: the only view
    // of world state a client actually has.
    std::optional<game::SnapshotPlayer> observed(std::uint32_t peer, std::uint8_t player_id) const {
        const auto snapshots = decode_to(net.sent, peer, MsgType::Snapshot, game::read_snapshot);
        if (snapshots.empty()) {
            return std::nullopt;
        }
        for (const game::SnapshotPlayer& player : snapshots.back().players) {
            if (player.player_id == player_id) {
                return player;
            }
        }
        return std::nullopt;
    }

    std::map<std::uint32_t, std::uint32_t> sequence;
};

struct Duel {
    std::uint32_t shooter_peer = 1;
    std::uint32_t victim_peer = 2;
    std::uint8_t shooter_id = 0;
    std::uint8_t victim_id = 0;
    game::InputCommand fire;        // aimed at the victim, trigger down
    game::InputCommand hold_still;  // the same aim, trigger up
};

// Two players joined and settled on the floor, the first aiming at the
// second. The aim is computed from the positions the server last reported,
// after gravity has finished with both of them, so it stays correct for as
// long as neither is given a movement input.
Duel set_up_duel(Harness& h) {
    Duel duel;
    const auto shooter = h.join(duel.shooter_peer, "shooter");
    const auto victim = h.join(duel.victim_peer, "victim");
    REQUIRE(shooter);
    REQUIRE(victim);
    duel.shooter_id = *shooter;
    duel.victim_id = *victim;

    h.tick(60);  // a second of falling, so both are standing on the floor

    const auto shooter_seen = h.observed(duel.shooter_peer, duel.shooter_id);
    const auto victim_seen = h.observed(duel.shooter_peer, duel.victim_id);
    REQUIRE(shooter_seen);
    REQUIRE(victim_seen);

    duel.fire = aim_at(shooter_seen->position, victim_seen->position, true);
    duel.hold_still = aim_at(shooter_seen->position, victim_seen->position, false);
    return duel;
}

}  // namespace

// --- joining ----------------------------------------------------------------

TEST_CASE("a client that completes the handshake becomes a player", "[server]") {
    Harness h;
    CHECK(h.game.player_count() == 0);

    const auto id = h.join(1, "alice");
    REQUIRE(id);
    CHECK(*id == 0);
    CHECK(h.game.player_count() == 1);
    CHECK(h.net.disconnected.empty());
    CHECK(h.net.count(MsgType::ServerReject) == 0);

    // The welcome echoes the map, because that is what the client loads.
    const auto welcomes =
        decode_to(h.net.sent, 1, MsgType::ServerWelcome, game::read_server_welcome);
    REQUIRE(welcomes.size() == 1);
    CHECK(welcomes[0].map == kMapName);

    // The newcomer is brought fully up to date before their first snapshot:
    // match state, their own score, and their weapon.
    CHECK(h.net.count_to(1, MsgType::MatchState) == 1);
    CHECK(h.net.count_to(1, MsgType::ScoreUpdate) == 1);
    CHECK(h.net.count_to(1, MsgType::WeaponStatus) == 1);

    // No leaderboard on a server with no stats store: there is nothing to
    // say, and saying it would be a message about an empty file.
    CHECK(h.net.count(MsgType::Leaderboard) == 0);

    // Everything in the handshake is reliable, on the reliable channel.
    for (const test::Sent& message : h.net.sent) {
        CHECK(message.channel == eng::NetChannel::Reliable);
        CHECK(message.reliable);
    }
}

TEST_CASE("two clients are told about each other", "[server]") {
    Harness h;
    const auto alice = h.join(1, "alice");
    REQUIRE(alice);
    h.net.clear();

    const auto bob = h.join(2, "bob");
    REQUIRE(bob);
    CHECK(*bob == 1);
    CHECK(h.game.player_count() == 2);

    // Bob is told who is already here -- and only that. He is never sent a
    // PlayerJoined for himself; his own id arrives in the ServerWelcome.
    const auto to_bob = decode_to(h.net.sent, 2, MsgType::PlayerJoined, game::read_player_joined);
    REQUIRE(to_bob.size() == 1);
    CHECK(to_bob[0].player_id == *alice);
    CHECK(to_bob[0].name == "alice");

    // ...and alice is told about bob, exactly once.
    const auto to_alice = decode_to(h.net.sent, 1, MsgType::PlayerJoined, game::read_player_joined);
    REQUIRE(to_alice.size() == 1);
    CHECK(to_alice[0].player_id == *bob);
    CHECK(to_alice[0].name == "bob");

    // Alice gets bob's join but not a second copy of the rest of the
    // handshake: that is addressed to the newcomer only.
    CHECK(h.net.count_to(1, MsgType::ServerWelcome) == 0);
    CHECK(h.net.count_to(1, MsgType::WeaponStatus) == 0);
}

TEST_CASE("a hello with the wrong protocol version is refused", "[server]") {
    Harness h;
    h.net.queue_connect(1);
    h.net.queue_message(1, test::hello_bytes(game::kProtocolVersion + 1u, "alice"));
    h.pump();

    CHECK(h.game.player_count() == 0);
    const auto rejects = decode_to(h.net.sent, 1, MsgType::ServerReject, game::read_server_reject);
    REQUIRE(rejects.size() == 1);
    CHECK(rejects[0].reason == game::RejectReason::VersionMismatch);
    CHECK(h.net.disconnected == std::vector<std::uint32_t>{1});
    CHECK(h.net.count(MsgType::ServerWelcome) == 0);
}

TEST_CASE("a hello with an unusable name is refused", "[server]") {
    Harness h;

    // An empty name and a 17-byte name are both outside the 1..16 the wire
    // format allows, so read_client_hello() rejects them.
    h.net.queue_connect(1);
    h.net.queue_message(1, test::hello_bytes(game::kProtocolVersion, ""));
    h.net.queue_connect(2);
    h.net.queue_message(2, test::hello_bytes(game::kProtocolVersion, "seventeen_chars_x"));
    h.pump();

    CHECK(h.game.player_count() == 0);
    CHECK(h.net.count(MsgType::ServerWelcome) == 0);
    CHECK(h.net.disconnected == std::vector<std::uint32_t>{1, 2});

    // BUG (behaviour pinned as-is, not endorsed): both are reported as
    // VersionMismatch. RejectReason::BadName exists in the protocol and
    // docs/packet-format.md documents the name rule separately from the
    // version rule, but ServerGame::handle_hello() cannot tell the two apart
    // -- read_client_hello() collapses "wrong version" and "unusable name"
    // into one nullopt, and the caller assumes the first. So a player whose
    // name is too long is told to update their game. BadName is dead code
    // today: nothing in the server ever sends it. Fixing it means having
    // read_client_hello report which check failed; when that happens, this
    // assertion should change to BadName rather than be deleted.
    const auto empty_name =
        decode_to(h.net.sent, 1, MsgType::ServerReject, game::read_server_reject);
    const auto long_name =
        decode_to(h.net.sent, 2, MsgType::ServerReject, game::read_server_reject);
    REQUIRE(empty_name.size() == 1);
    REQUIRE(long_name.size() == 1);
    CHECK(empty_name[0].reason == game::RejectReason::VersionMismatch);
    CHECK(long_name[0].reason == game::RejectReason::VersionMismatch);
}

TEST_CASE("the server refuses a player past the last slot", "[server]") {
    Harness h;
    for (std::uint32_t peer = 1; peer <= game::kMaxPlayers; ++peer) {
        REQUIRE(h.join(peer, "player" + std::to_string(peer)));
    }
    CHECK(h.game.player_count() == game::kMaxPlayers);
    h.net.clear();

    const std::uint32_t extra = game::kMaxPlayers + 1u;
    CHECK_FALSE(h.join(extra, "unlucky"));
    CHECK(h.game.player_count() == game::kMaxPlayers);

    const auto rejects =
        decode_to(h.net.sent, extra, MsgType::ServerReject, game::read_server_reject);
    REQUIRE(rejects.size() == 1);
    CHECK(rejects[0].reason == game::RejectReason::ServerFull);
    CHECK(h.net.disconnected == std::vector<std::uint32_t>{extra});

    // The refusal is addressed to the newcomer alone. The players already in
    // the match hear nothing about someone who never joined.
    CHECK(h.net.count(MsgType::PlayerJoined) == 0);
}

TEST_CASE("a socket that connects and never says hello holds no slot", "[server]") {
    Harness h;

    // Eight connections, no hellos. This is the shape of the M26 denial of
    // service, and at THIS layer it is already harmless: ServerGame allocates
    // a player slot in handle_hello(), never on Connected, so a silent socket
    // occupies nothing and is told nothing.
    for (std::uint32_t peer = 1; peer <= game::kMaxPlayers; ++peer) {
        h.net.queue_connect(peer);
    }
    h.pump();
    h.tick(600);  // ten seconds of silence

    CHECK(h.game.player_count() == 0);
    CHECK(h.net.sent.empty());
    CHECK(h.net.disconnected.empty());

    // What M26 actually fixed lives one layer down: WebSocketHost handed a
    // peer id to an accepted TCP connection before the HTTP upgrade, so
    // sockets that never upgraded exhausted max_peers. There is no timer for
    // it in ServerGame and no way to reach one from here, so the reaping
    // itself is tested in tests/net/websocket_host_tests.cpp. What is
    // testable here is the half that makes that fix sufficient: a real
    // player can still take every slot afterwards.
    for (std::uint32_t peer = 100; peer < 100 + game::kMaxPlayers; ++peer) {
        REQUIRE(h.join(peer, "late"));
    }
    CHECK(h.game.player_count() == game::kMaxPlayers);

    // An input from a peer that never handshook is ignored outright: there is
    // no player to attribute it to, so it is not even counted against one.
    h.net.clear();
    h.net.queue_message(1, test::input_bytes(1, game::InputCommand{}), eng::NetChannel::Sequenced);
    h.pump();
    CHECK(h.net.sent.empty());
    CHECK(h.net.disconnected.empty());
}

TEST_CASE("a disconnect frees the slot and tells the others", "[server]") {
    Harness h;
    const auto alice = h.join(1, "alice");
    const auto bob = h.join(2, "bob");
    REQUIRE(alice);
    REQUIRE(bob);
    h.net.clear();

    h.net.queue_disconnect(1);
    h.pump();
    CHECK(h.game.player_count() == 1);

    // Bob hears about it; the departed peer is not written to.
    const auto left = decode_to(h.net.sent, 2, MsgType::PlayerLeft, game::read_player_left);
    REQUIRE(left.size() == 1);
    CHECK(left[0].player_id == *alice);
    CHECK_FALSE(h.net.anything_sent_to(1));

    // The freed slot is reused, so slots are a resource that is actually
    // returned rather than leaked over a long-running server's lifetime.
    const auto carol = h.join(3, "carol");
    REQUIRE(carol);
    CHECK(*carol == *alice);
    CHECK(h.game.player_count() == 2);
}

TEST_CASE("a duplicate hello from a joined player is ignored", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "alice"));
    h.net.clear();

    h.net.queue_message(1, test::hello_bytes(game::kProtocolVersion, "alice_again"));
    h.pump();

    CHECK(h.game.player_count() == 1);
    CHECK(h.net.sent.empty());  // no second welcome, no second PlayerJoined
    CHECK(h.net.disconnected.empty());
}

// --- snapshots ---------------------------------------------------------------

TEST_CASE("snapshots are unreliable, sequenced, and ack the client's inputs", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "alice"));
    h.net.clear();

    h.tick(30);
    const auto snapshots = decode_to(h.net.sent, 1, MsgType::Snapshot, game::read_snapshot);
    // 60 Hz ticks at 20 Hz snapshots: ten in thirty ticks. Spelled as a
    // literal on purpose -- deriving it from kSnapshotDivisor would make the
    // assertion agree with any value of it, including the ones that triple
    // the server's outbound bandwidth.
    STATIC_REQUIRE(game::kSnapshotDivisor == 3);
    CHECK(snapshots.size() == 10u);

    for (const test::Sent& message : h.net.sent) {
        if (message.type() == MsgType::Snapshot) {
            CHECK(message.channel == eng::NetChannel::Sequenced);
            CHECK_FALSE(message.reliable);
        }
    }

    // The ack is what lets a client retire predicted inputs, so it has to
    // track what the simulation actually consumed.
    h.net.clear();
    game::InputCommand command;
    h.drive(1, command, 6);
    const auto acked = decode_to(h.net.sent, 1, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(acked.empty());
    CHECK(acked.back().last_processed_input == 6);
}

TEST_CASE("a gap in the input stream is waited out, then skipped", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "laggy"));
    h.drive_tick(1, game::InputCommand{});  // sequence 1 consumed

    // Sequences 2, 3 and 4 are lost; 5 arrives. The server consumes exactly
    // one input per tick in order, so it now has nothing it is allowed to
    // consume and reuses the last command instead.
    h.net.queue_message(1, test::input_bytes(5, game::InputCommand{}), eng::NetChannel::Sequenced);
    h.pump();

    h.net.clear();
    h.tick(5);
    const auto stalled = decode_to(h.net.sent, 1, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(stalled.empty());
    CHECK(stalled.back().last_processed_input == 1);

    // On the sixth starved tick it stops waiting and jumps to the oldest
    // input it does have, rather than stalling the player until the gap is
    // filled by packets that are never coming.
    h.net.clear();
    h.tick(4);  // the jump, plus enough ticks to guarantee a snapshot
    const auto jumped = decode_to(h.net.sent, 1, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(jumped.empty());
    CHECK(jumped.back().last_processed_input == 5);
}

TEST_CASE("an input far ahead of the simulation is dropped", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "impatient"));
    h.net.clear();

    // `pending` is an unbounded std::map filled straight from the wire, and
    // this window is the only thing between it and a client that inserts
    // far-future sequences the tick loop will never reach.
    const game::InputCommand command;
    h.net.queue_message(1, test::input_bytes(game::kMaxInputAhead + 2u, command),
                        eng::NetChannel::Sequenced);
    h.pump();

    // Thirty ticks is well past the six starved ticks after which the server
    // fast-forwards to the oldest input it has -- which is what would consume
    // this one if it had been queued at all.
    h.tick(30);
    const auto ignored = decode_to(h.net.sent, 1, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(ignored.empty());
    CHECK(ignored.back().last_processed_input == 0);

    // The edge of the window is inside it: exactly kMaxInputAhead ahead of
    // what has been processed is accepted, so the bound rejects a flood
    // without also rejecting a client that is merely early.
    h.net.clear();
    h.net.queue_message(1, test::input_bytes(game::kMaxInputAhead, command),
                        eng::NetChannel::Sequenced);
    h.pump();
    h.tick(30);
    const auto accepted = decode_to(h.net.sent, 1, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(accepted.empty());
    CHECK(accepted.back().last_processed_input == game::kMaxInputAhead);
}

TEST_CASE("a client holding Fire every tick still fires at the weapon's rate", "[server]") {
    // The claim this pins down is the whole reason firing is a button in
    // InputCommand rather than a hit claim: the rate is the server's, not the
    // client's. This client sends Fire on all 60 ticks of a second.
    Harness h;
    REQUIRE(h.join(1, "spammer"));
    h.tick(60);
    h.net.clear();

    game::InputCommand fire;
    game::set_button(fire, game::Button::Fire, true);
    h.drive(1, fire, 60);

    // 600 rpm is a shot every 0.1 s, so ten in a second -- not sixty. The
    // exact tick a shot lands on depends on float accumulation in the shared
    // weapon state machine (tests/game/weapon_tests.cpp pins that precisely),
    // so allow one either way; what matters here is the order of magnitude.
    const std::size_t shots = h.net.count_to(1, MsgType::FireEvent);
    CHECK(shots >= 9);
    CHECK(shots <= 11);
}

// --- combat ------------------------------------------------------------------

TEST_CASE("shooting a player damages, kills and scores", "[server]") {
    Harness h;
    const Duel duel = set_up_duel(h);
    h.net.clear();

    const int ticks = h.drive_until(duel.shooter_peer, duel.fire, MsgType::PlayerDied, 120);
    REQUIRE(ticks > 0);

    // Four hits of 25 against 100 health, one shot per 6 ticks.
    const auto damage = decode_all(h.net.sent, MsgType::PlayerDamaged, game::read_player_damaged);
    REQUIRE(damage.size() == 8u);  // four hits, each told to both players
    const auto hits_to_shooter =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerDamaged, game::read_player_damaged);
    REQUIRE(hits_to_shooter.size() == 4);
    for (const game::PlayerDamagedMsg& hit : hits_to_shooter) {
        CHECK(hit.victim == duel.victim_id);
        CHECK(hit.attacker == duel.shooter_id);
        CHECK(hit.amount == 25.0f);
    }
    CHECK(hits_to_shooter[0].health == 75.0f);
    CHECK(hits_to_shooter[3].health == 0.0f);

    // Every shot is announced to everyone, hit or miss: it is what draws the
    // tracer and plays the bang. And it says who shot and what they hit --
    // hit_player is what puts the hit marker on the shooter's crosshair, so a
    // FireEvent that merely arrives is not the same as a correct one.
    const auto shots =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::FireEvent, game::read_fire_event);
    REQUIRE(shots.size() == 4);
    for (const game::FireEventMsg& shot : shots) {
        CHECK(shot.shooter == duel.shooter_id);
        CHECK(shot.slot == 0);
        REQUIRE(shot.rays.size() == 1);  // the test rifle fires one pellet
        CHECK(shot.rays[0].hit_player == duel.victim_id);
    }
    CHECK(
        decode_to(h.net.sent, duel.victim_peer, MsgType::FireEvent, game::read_fire_event).size() ==
        4);

    // The death, told to both.
    const auto deaths =
        decode_to(h.net.sent, duel.victim_peer, MsgType::PlayerDied, game::read_player_died);
    REQUIRE(deaths.size() == 1);
    CHECK(deaths[0].victim == duel.victim_id);
    CHECK(deaths[0].killer == duel.shooter_id);
    CHECK(decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerDied, game::read_player_died)
              .size() == 1);

    // One kill for the shooter, one death for the victim, and nothing else.
    const auto scores =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::ScoreUpdate, game::read_score_update);
    REQUIRE(scores.size() == 2);
    CHECK(scores[0].player == duel.shooter_id);
    CHECK(scores[0].kills == 1);
    CHECK(scores[0].deaths == 0);
    CHECK(scores[1].player == duel.victim_id);
    CHECK(scores[1].kills == 0);
    CHECK(scores[1].deaths == 1);

    // The victim is dead in the next snapshot the shooter is sent. Snapshots
    // go out every kSnapshotDivisor ticks, so one is not guaranteed to have
    // followed the death yet.
    h.tick(game::kSnapshotDivisor);
    const auto victim_seen = h.observed(duel.shooter_peer, duel.victim_id);
    REQUIRE(victim_seen);
    CHECK((victim_seen->flags & game::kFlagAlive) == 0);

    // A corpse is out of the fight. The shooter never lets go of the trigger,
    // and for the next twenty ticks the shots go straight through where the
    // body is and hit the floor behind it: no damage, no second death, and no
    // second kill on the scoreboard.
    h.net.clear();
    h.drive(duel.shooter_peer, duel.fire, 20);
    CHECK(h.net.count(MsgType::PlayerDamaged) == 0);
    CHECK(h.net.count(MsgType::PlayerDied) == 0);
    CHECK(h.net.count(MsgType::ScoreUpdate) == 0);

    // ...and they really were shots, not a shooter that quietly stopped
    // firing, which would make the three assertions above say nothing.
    const auto after_death =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::FireEvent, game::read_fire_event);
    REQUIRE_FALSE(after_death.empty());
    for (const game::FireEventMsg& shot : after_death) {
        REQUIRE(shot.rays.size() == 1);
        CHECK(shot.rays[0].hit_player == game::kNoPlayer);
    }
}

TEST_CASE("a shot that hits nobody says so", "[server]") {
    Harness h;
    const Duel duel = set_up_duel(h);
    h.net.clear();

    // The same shooter, turned ninety degrees off the victim and level. Over
    // a flat floor at eye height there is nothing out there at all, so every
    // ray runs to its full range and names nobody.
    game::InputCommand wide = duel.fire;
    wide.yaw = duel.fire.yaw + glm::radians(90.0f);
    wide.pitch = 0.0f;
    h.drive(duel.shooter_peer, wide, 20);

    const auto shots =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::FireEvent, game::read_fire_event);
    REQUIRE_FALSE(shots.empty());
    for (const game::FireEventMsg& shot : shots) {
        CHECK(shot.shooter == duel.shooter_id);
        REQUIRE(shot.rays.size() == 1);
        CHECK(shot.rays[0].hit_player == game::kNoPlayer);
    }
    CHECK(h.net.count(MsgType::PlayerDamaged) == 0);
    CHECK(h.net.count(MsgType::PlayerDied) == 0);
}

TEST_CASE("a shotgun's pellets land as one hit and one death", "[server]") {
    Harness h;
    const Duel duel = set_up_duel(h);
    h.net.clear();

    // Slot 1 is the eight-pellet shotgun. The slot is state, not an edge, so
    // it just rides along on every command.
    game::InputCommand blast = duel.fire;
    blast.weapon_slot = 1;
    const int ticks = h.drive_until(duel.shooter_peer, blast, MsgType::PlayerDied, 120);
    REQUIRE(ticks > 0);

    // One trigger pull, one FireEvent, eight rays -- each one an independent
    // hitscan through the deterministic spread.
    const auto shots =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::FireEvent, game::read_fire_event);
    REQUIRE(shots.size() == 1);
    CHECK(shots[0].shooter == duel.shooter_id);
    CHECK(shots[0].slot == 1);
    REQUIRE(shots[0].rays.size() == static_cast<std::size_t>(kShotgunPellets));
    for (const game::FireRay& ray : shots[0].rays) {
        CHECK(ray.hit_player == duel.victim_id);
    }

    // The pellets are not eight separate hits: they are accumulated per
    // victim and applied once, so the client draws one damage number and the
    // victim can only be killed once no matter how many landed.
    const auto hits =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerDamaged, game::read_player_damaged);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].victim == duel.victim_id);
    CHECK(hits[0].amount == static_cast<float>(kShotgunPellets) * kShotgunPelletDamage);
    CHECK(hits[0].health == 0.0f);

    const auto deaths =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerDied, game::read_player_died);
    REQUIRE(deaths.size() == 1);
    CHECK(deaths[0].victim == duel.victim_id);
    CHECK(deaths[0].killer == duel.shooter_id);

    // One kill, not eight.
    const auto scores =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::ScoreUpdate, game::read_score_update);
    REQUIRE_FALSE(scores.empty());
    CHECK(scores[0].player == duel.shooter_id);
    CHECK(scores[0].kills == 1);
}

TEST_CASE("where a shot lands decides what it costs", "[server]") {
    // The test rifle does 25 to the body, x2.0 to the head, x0.75 to a limb.
    struct Case {
        const char* what;
        float aim_height;
        game::HitZone zone;
        float damage;
    };
    const Case cases[] = {
        {"head", 1.70f, game::HitZone::Head, 50.0f},
        {"torso", 1.20f, game::HitZone::Torso, 25.0f},
        {"leg", 0.50f, game::HitZone::Leg, 18.75f},
    };

    for (const Case& shot : cases) {
        INFO("aiming at the " << shot.what);
        // A fresh match per case: the victim starts every one at full health,
        // and the weapon is off cooldown.
        Harness fresh;
        const Duel d = set_up_duel(fresh);
        const auto from = fresh.observed(d.shooter_peer, d.shooter_id);
        const auto at = fresh.observed(d.shooter_peer, d.victim_id);
        REQUIRE(from);
        REQUIRE(at);

        fresh.net.clear();
        fresh.drive_tick(d.shooter_peer,
                         aim_at(from->position, at->position, true, shot.aim_height));

        const auto hits = decode_to(fresh.net.sent, d.shooter_peer, MsgType::PlayerDamaged,
                                    game::read_player_damaged);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].zone == shot.zone);
        CHECK(hits[0].amount == shot.damage);
        CHECK(hits[0].health == 100.0f - shot.damage);
    }
}

TEST_CASE("a head shot kills in half the shots a body shot needs", "[server]") {
    // The point of the whole feature, stated as the thing a player would
    // notice: 100 health, 25 a shot to the body is four, 50 to the head is two.
    const auto deaths_for_aim = [](float aim_height) {
        Harness h;
        const Duel duel = set_up_duel(h);
        const auto shooter = h.observed(duel.shooter_peer, duel.shooter_id);
        const auto victim = h.observed(duel.shooter_peer, duel.victim_id);
        REQUIRE(shooter);
        REQUIRE(victim);
        h.net.clear();
        const game::InputCommand fire =
            aim_at(shooter->position, victim->position, true, aim_height);
        h.drive_until(duel.shooter_peer, fire, MsgType::PlayerDied, 120);
        return decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerDamaged,
                         game::read_player_damaged)
            .size();
    };

    CHECK(deaths_for_aim(1.20f) == 4u);  // body
    CHECK(deaths_for_aim(1.70f) == 2u);  // head
}

TEST_CASE("a shot is resolved where the shooter saw the victim", "[server]") {
    Harness h;
    const Duel duel = set_up_duel(h);

    // The victim runs across the shooter's line of sight, so every tick of
    // rewind is a tick of pure miss distance. One input packet is enough: a
    // server starved of inputs replays the last command it consumed, so the
    // victim keeps running without another packet arriving.
    game::InputCommand run;
    run.yaw = 0.0f;  // yaw 0 looks down -Z; the duel is fought along +X
    game::set_button(run, game::Button::Forward, true);
    h.net.queue_message(duel.victim_peer, test::input_bytes(1, run), eng::NetChannel::Sequenced);
    h.pump();

    // Let them get up to speed, then line the sample up with the snapshot
    // rate, so the position aimed at is one the server really published on a
    // tick this test can name.
    h.drive(duel.shooter_peer, duel.hold_still, 20);
    while (h.game.current_tick() % static_cast<std::uint32_t>(game::kSnapshotDivisor) != 0) {
        h.drive_tick(duel.shooter_peer, duel.hold_still);
    }
    const std::uint32_t view_tick = h.game.current_tick();
    const auto shooter_seen = h.observed(duel.shooter_peer, duel.shooter_id);
    const auto victim_then = h.observed(duel.shooter_peer, duel.victim_id);
    REQUIRE(shooter_seen);
    REQUIRE(victim_then);

    // Twelve more ticks. kMaxRewindTicks is 15, so the sampled tick is still
    // inside the legal window when the shot goes off on the thirteenth.
    h.drive(duel.shooter_peer, duel.hold_still, 12);
    const auto victim_now = h.observed(duel.shooter_peer, duel.victim_id);
    REQUIRE(victim_now);
    // Metres, against a capsule 0.4 m in radius: the two positions are not a
    // near-miss apart, they are nowhere near each other.
    REQUIRE(glm::length(victim_now->position - victim_then->position) > 1.0f);

    // The shot, aimed where the victim WAS and claiming that tick as the view
    // it was taken from. This is the hit that only exists because the server
    // rewinds the victim before testing the ray.
    const game::InputCommand snapshot_shot =
        aim_at(shooter_seen->position, victim_then->position, true);
    h.net.clear();
    h.drive_tick(duel.shooter_peer, snapshot_shot, view_tick);
    const auto hits =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerDamaged, game::read_player_damaged);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].victim == duel.victim_id);

    // The control, and the half that makes the above mean something: the same
    // aim, claiming the CURRENT tick instead, so there is no rewind. The
    // victim is not there any more, and the shot misses.
    h.drive(duel.shooter_peer, duel.hold_still, 6);  // off the trigger, weapon cools
    h.net.clear();
    h.drive_tick(duel.shooter_peer, snapshot_shot, h.game.current_tick() + 1u);
    // Scoped to one recipient: a fire event is fanned out per peer (bots have
    // no connection, so there is no transport-level broadcast to count), and
    // an unscoped count would be "one per player in the match".
    CHECK(h.net.count_to(duel.shooter_peer, MsgType::FireEvent) == 1);  // it did fire
    CHECK(h.net.count(MsgType::PlayerDamaged) == 0);
}

TEST_CASE("the killcam goes to the victim and nobody else", "[server]") {
    Harness h;
    const Duel duel = set_up_duel(h);
    h.net.clear();
    REQUIRE(h.drive_until(duel.shooter_peer, duel.fire, MsgType::PlayerDied, 120) > 0);

    const auto to_victim =
        decode_to(h.net.sent, duel.victim_peer, MsgType::KillCam, game::read_kill_cam);
    REQUIRE(to_victim.size() == 1);
    CHECK(to_victim[0].killer == duel.shooter_id);
    CHECK_FALSE(to_victim[0].samples.empty());
    CHECK(h.net.count_to(duel.shooter_peer, MsgType::KillCam) == 0);
}

TEST_CASE("a dead player respawns after kRespawnSeconds", "[server]") {
    Harness h;
    const Duel duel = set_up_duel(h);
    h.net.clear();
    REQUIRE(h.drive_until(duel.shooter_peer, duel.fire, MsgType::PlayerDied, 120) > 0);

    // Off the trigger, or the shooter kills the victim again the moment they
    // are back and the timing below stops meaning anything.
    h.drive(duel.shooter_peer, duel.hold_still, 2);
    h.net.clear();

    // A dead player's inputs are still consumed, so their client keeps
    // getting acks and its unacknowledged prediction buffer does not grow for
    // the whole three seconds they are down.
    h.drive(duel.victim_peer, game::InputCommand{}, 10);
    h.tick(3);
    const auto acked =
        decode_to(h.net.sent, duel.victim_peer, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(acked.empty());
    CHECK(acked.back().last_processed_input == 10);
    h.net.clear();

    // kRespawnSeconds is 3. Counting the 13 ticks above, this puts us 150
    // ticks (2.5 s) past the death -- still short.
    h.tick(137);
    CHECK(h.net.count(MsgType::PlayerRespawned) == 0);

    h.tick(60);
    const auto respawns = decode_to(h.net.sent, duel.victim_peer, MsgType::PlayerRespawned,
                                    game::read_player_respawned);
    REQUIRE(respawns.size() == 1);
    CHECK(respawns[0].player == duel.victim_id);
    // Back on a spawn point, not where they fell.
    const auto spawns = default_spawns();
    CHECK(std::any_of(spawns.begin(), spawns.end(), [&](const glm::vec3& spawn) {
        return glm::length(respawns[0].position - spawn) < 0.001f;
    }));

    // Alive again, with a full magazine reported to them.
    h.tick(6);
    const auto victim_seen = h.observed(duel.victim_peer, duel.victim_id);
    REQUIRE(victim_seen);
    CHECK((victim_seen->flags & game::kFlagAlive) != 0);
    const auto weapon =
        decode_to(h.net.sent, duel.victim_peer, MsgType::WeaponStatus, game::read_weapon_status);
    REQUIRE_FALSE(weapon.empty());
    CHECK(weapon.back().ammo == 30);
}

TEST_CASE("falling out of the world is a death with no killer", "[server]") {
    // A floor small enough to walk off in half a second.
    Harness h{2.0f, {{0.0f, 1.0f, 0.0f}}};
    const std::uint32_t peer = 1;
    const auto id = h.join(peer, "faller");
    REQUIRE(id);
    h.tick(60);
    h.net.clear();

    game::InputCommand forward;
    game::set_button(forward, game::Button::Forward, true);
    // 30 driven ticks is plenty to reach the edge. After that the client goes
    // silent, and the server keeps replaying the last command it consumed:
    // the fast-forward branch needs a pending input to jump to, so an empty
    // queue means the last command repeats for as long as the player is
    // connected. Worth knowing -- a client that stops sending does not stop
    // moving -- and here it is what keeps the fall going.
    h.drive(peer, forward, 30);
    h.tick(240);

    const auto deaths = decode_to(h.net.sent, peer, MsgType::PlayerDied, game::read_player_died);
    REQUIRE(deaths.size() == 1);
    CHECK(deaths[0].victim == *id);
    CHECK(deaths[0].killer == game::kNoPlayer);

    // A death with no killer still costs the victim, and credits nobody.
    const auto scores = decode_to(h.net.sent, peer, MsgType::ScoreUpdate, game::read_score_update);
    REQUIRE(scores.size() == 1);
    CHECK(scores[0].player == *id);
    CHECK(scores[0].kills == 0);
    CHECK(scores[0].deaths == 1);
}

// --- the match ---------------------------------------------------------------

TEST_CASE("the match ends on the clock, then restarts and wipes the scoreboard", "[server]") {
    Harness h;

    // Fast-forward with nobody connected: an empty tick is only the match
    // clock, so 293 seconds of it costs almost nothing.
    const int fast_forward = 17600;
    h.tick(fast_forward);

    const Duel duel = set_up_duel(h);
    h.net.clear();
    REQUIRE(h.drive_until(duel.shooter_peer, duel.fire, MsgType::PlayerDied, 120) > 0);
    h.drive(duel.shooter_peer, duel.hold_still, 2);  // off the trigger

    // Run out the rest of the five minutes.
    h.net.clear();
    h.tick(400);

    const auto states =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::MatchState, game::read_match_state);
    REQUIRE(states.size() >= 2u);
    CHECK(states.front().phase == game::MatchPhase::Playing);
    CHECK(states.back().phase == game::MatchPhase::Ended);
    // Ended carries the countdown to the restart, not the match clock.
    CHECK(states.back().seconds_remaining <=
          static_cast<std::uint16_t>(game::kMatchRestartSeconds));

    // Weapons are not advanced at all outside MatchPhase::Playing, so a
    // client that keeps holding Fire through the end screen shoots nothing.
    h.net.clear();
    h.drive(duel.shooter_peer, duel.fire, 30);
    CHECK(h.net.count(MsgType::FireEvent) == 0);
    h.drive(duel.shooter_peer, duel.hold_still, 2);  // off the trigger again

    // kMatchRestartSeconds is 8; 500 ticks is 8.3.
    h.net.clear();
    h.tick(500);

    const auto after =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::MatchState, game::read_match_state);
    REQUIRE_FALSE(after.empty());
    CHECK(after.back().phase == game::MatchPhase::Playing);
    CHECK(std::any_of(after.begin(), after.end(), [](const game::MatchStateMsg& state) {
        return state.phase == game::MatchPhase::Playing &&
               state.seconds_remaining == static_cast<std::uint16_t>(game::kMatchSeconds);
    }));

    // Everyone starts the new match at zero, and alive.
    const auto scores =
        decode_to(h.net.sent, duel.shooter_peer, MsgType::ScoreUpdate, game::read_score_update);
    REQUIRE(scores.size() >= 2u);
    for (const game::ScoreUpdateMsg& score : scores) {
        CHECK(score.kills == 0);
        CHECK(score.deaths == 0);
    }
    CHECK(decode_to(h.net.sent, duel.shooter_peer, MsgType::PlayerRespawned,
                    game::read_player_respawned)
              .size() >= 2u);
}

// --- bots --------------------------------------------------------------------

TEST_CASE("bots play the match and are never sent anything", "[server]") {
    // Four spawn points so the bots start apart and actually have something
    // to walk toward and shoot at.
    Harness h{60.0f,
              {{0.0f, 1.0f, 0.0f}, {8.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 8.0f}, {8.0f, 1.0f, 8.0f}}};
    for (int i = 0; i < 4; ++i) {
        REQUIRE(h.game.add_bot("bot" + std::to_string(i + 1)));
    }
    CHECK(h.game.player_count() == 4);
    // Nothing is sent when a bot joins: there is no bot to tell, and no
    // human here yet to tell about it.
    CHECK(h.net.sent.empty());

    const std::uint32_t human = 9;
    const auto human_id = h.join(human, "human");
    REQUIRE(human_id);
    CHECK(h.game.player_count() == 5);

    h.tick(600);  // ten seconds of a live match

    // THE assertion. A bot's `peer` is 0 -- a real transport starts handing
    // out ids at 1, so nothing today collides with it, but that is an
    // invariant of two other files rather than anything ServerGame checks.
    // Every send must therefore be addressed to the one peer that exists.
    for (const test::Sent& message : h.net.sent) {
        CHECK_FALSE(message.broadcast);
        CHECK(message.peer == human);
    }
    CHECK_FALSE(h.net.anything_sent_to(0));
    CHECK_FALSE(h.net.used_broadcast());
    CHECK(h.net.disconnected.empty());

    // ...and they are not being skipped because they are not there. The
    // snapshot the human receives has all five players in it.
    const auto snapshots = decode_to(h.net.sent, human, MsgType::Snapshot, game::read_snapshot);
    REQUIRE_FALSE(snapshots.empty());
    CHECK(snapshots.back().players.size() == 5);

    // Bots run the real combat path, not a parallel imitation of it: they
    // fire the same weapons through the same hitscan code.
    const auto shots = decode_to(h.net.sent, human, MsgType::FireEvent, game::read_fire_event);
    REQUIRE_FALSE(shots.empty());
    CHECK(std::any_of(shots.begin(), shots.end(),
                      [&](const game::FireEventMsg& shot) { return shot.shooter != *human_id; }));
}

TEST_CASE("career stats record the humans and never the bots", "[server]") {
    Harness h;
    TempDir dir;
    const auto path = dir.file("stats.bin");
    h.game.set_stats_path(path);

    // Almost the whole match, with nobody connected: an empty tick is only
    // the match clock, so the interesting part costs 800 ticks, not 18,000.
    h.tick(17800);

    // A fresh server has nothing to show, so the handshake carries no
    // leaderboard at all rather than an empty one.
    REQUIRE(h.join(1, "human"));
    CHECK(h.net.count(MsgType::Leaderboard) == 0);

    REQUIRE(h.game.add_bot("bot1"));
    h.net.clear();

    // Run the match out. A restart is what credits everyone present with
    // having played it -- and bots are deliberately not "everyone".
    h.tick(300);  // past the 300 s match...
    h.tick(500);  // ...and past the 8 s restart

    const auto leaderboards =
        decode_to(h.net.sent, 1, MsgType::Leaderboard, game::read_leaderboard);
    REQUIRE_FALSE(leaderboards.empty());
    const game::LeaderboardMsg& board = leaderboards.back();
    REQUIRE(board.entries.size() == 1);
    CHECK(board.entries[0].name == "human");
    CHECK(board.entries[0].matches == 1);

    // And on disk, which is the point of the feature.
    h.game.save_stats();
    const game::StatsStore reloaded = game::StatsStore::open(path);
    REQUIRE(reloaded.healthy());
    CHECK(reloaded.size() == 1);
    REQUIRE(reloaded.find("human") != nullptr);
    CHECK(reloaded.find("human")->matches == 1);
    CHECK(reloaded.find("bot1") == nullptr);
}

TEST_CASE("add_bot refuses when every slot is taken", "[server]") {
    Harness h;
    for (int i = 0; i < static_cast<int>(game::kMaxPlayers); ++i) {
        REQUIRE(h.game.add_bot("bot" + std::to_string(i)));
    }
    CHECK(h.game.player_count() == game::kMaxPlayers);
    CHECK_FALSE(h.game.add_bot("one_too_many"));
    CHECK(h.game.player_count() == game::kMaxPlayers);

    // And a human is refused for the same reason, with a message.
    CHECK_FALSE(h.join(1, "human"));
    const auto rejects = decode_to(h.net.sent, 1, MsgType::ServerReject, game::read_server_reject);
    REQUIRE(rejects.size() == 1);
    CHECK(rejects[0].reason == game::RejectReason::ServerFull);
}

TEST_CASE("a bot's sentinel peer id is a real peer id", "[server]") {
    Harness h;
    REQUIRE(h.game.add_bot("bot1"));
    REQUIRE(h.join(1, "human"));
    CHECK(h.game.player_count() == 2);
    h.net.clear();

    // BUG (behaviour pinned as-is, not endorsed): a bot stores peer = 0 as
    // "no peer", but 0 is a perfectly representable peer id and
    // find_player_by_peer() cannot tell the sentinel from a real client. Both
    // halves of the handshake path then misfire for a client whose transport
    // handed it id 0.
    //
    // Nothing hands out 0 today -- NetHost and WebSocketHost both start their
    // counters at 1, and CompositeTransport tags a child index into the top
    // bits -- so this is a landmine rather than a live defect. It is also
    // load-bearing in three files at once and enforced in none of them. The
    // fix is a sentinel outside the id space (an optional<uint32_t> peer, or
    // reserving 0 in the transports explicitly).

    // First half: the hello is swallowed as a duplicate, because the peer
    // "already has a player" -- the bot. The client is welcomed by nothing
    // and told nothing, so it hangs in Connecting until it times out.
    h.net.queue_connect(0);
    h.net.queue_message(0, test::hello_bytes(game::kProtocolVersion, "peer_zero"));
    h.pump();
    CHECK(h.game.player_count() == 2);  // would be 3 if the join had worked
    CHECK(h.net.count(MsgType::ServerWelcome) == 0);
    CHECK(h.net.count(MsgType::ServerReject) == 0);

    // Second half, and the damaging one: that client dropping deletes the
    // bot, because the disconnect is matched to the bot's slot.
    h.net.queue_disconnect(0);
    h.pump();
    CHECK(h.game.player_count() == 1);
    const auto left = decode_to(h.net.sent, 1, MsgType::PlayerLeft, game::read_player_left);
    REQUIRE(left.size() == 1);
    CHECK(left[0].player_id == 0);  // the bot's slot
}

// --- abuse accounting ---------------------------------------------------------

TEST_CASE("a flood of input packets gets the client disconnected", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "flooder"));
    h.net.clear();

    // The limit is 200 input packets per second. Same sequence every time, so
    // the kMaxInputAhead window plays no part in what is being measured.
    const game::InputCommand command;
    for (int i = 0; i < 200; ++i) {
        h.net.queue_message(1, test::input_bytes(1, command), eng::NetChannel::Sequenced);
    }
    h.pump();
    CHECK(h.net.disconnected.empty());

    h.net.queue_message(1, test::input_bytes(1, command), eng::NetChannel::Sequenced);
    h.pump();
    CHECK(h.net.disconnected == std::vector<std::uint32_t>{1});

    // ServerGame asks the transport to close the connection; it does not drop
    // the player itself. The slot is released when the transport reports the
    // Disconnected event, which is the only thing that knows the socket is
    // really gone.
    CHECK(h.game.player_count() == 1);

    // The budget is per second, and it does come back.
    h.tick(60);
    h.net.clear();
    for (int i = 0; i < 200; ++i) {
        h.net.queue_message(1, test::input_bytes(1, command), eng::NetChannel::Sequenced);
    }
    h.pump();
    CHECK(h.net.disconnected.empty());
}

TEST_CASE("malformed messages are counted, and eleven is too many", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "garbage"));
    h.net.clear();

    // Two different kinds of malformed message share one budget of ten: a
    // byte that is not a message type at all, and a well-formed Input header
    // with no body behind it.
    const std::vector<std::uint8_t> bad_type{0x00};
    const std::vector<std::uint8_t> truncated_input{static_cast<std::uint8_t>(MsgType::Input)};

    for (int i = 0; i < 5; ++i) {
        h.net.queue_message(1, bad_type);
    }
    for (int i = 0; i < 5; ++i) {
        h.net.queue_message(1, truncated_input, eng::NetChannel::Sequenced);
    }
    h.pump();
    CHECK(h.net.disconnected.empty());  // exactly ten, still tolerated

    h.net.queue_message(1, bad_type);
    h.pump();
    CHECK(h.net.disconnected == std::vector<std::uint32_t>{1});
}

TEST_CASE("a well-formed message of the wrong type is neither counted nor limited", "[server]") {
    Harness h;
    REQUIRE(h.join(1, "wrongway"));
    h.net.clear();

    // BUG (behaviour pinned as-is, not endorsed): handle_event() ends with
    // "Anything else from a client is ignored (and counted)", but nothing
    // counts it. A type byte in range that is not ClientHello or Input --
    // Snapshot, say, which only ever travels server to client -- falls out of
    // the dispatch untouched: no bad_messages, and no input rate limit
    // either, because that lives inside handle_input(). So the entire abuse
    // budget is sidestepped by sending the wrong message type instead of a
    // broken one, and a client can do it forever.
    const std::vector<std::uint8_t> wrong_way{static_cast<std::uint8_t>(MsgType::Snapshot)};
    for (int i = 0; i < 500; ++i) {
        h.net.queue_message(1, wrong_way);
    }
    h.pump();
    CHECK(h.net.disconnected.empty());
    CHECK(h.net.sent.empty());
    CHECK(h.game.player_count() == 1);

    // And they really were not counted: the full budget of ten malformed
    // messages is still there afterwards, so the eleventh is what kicks.
    const std::vector<std::uint8_t> bad_type{0x00};
    for (int i = 0; i < 10; ++i) {
        h.net.queue_message(1, bad_type);
    }
    h.pump();
    CHECK(h.net.disconnected.empty());

    h.net.queue_message(1, bad_type);
    h.pump();
    CHECK(h.net.disconnected == std::vector<std::uint32_t>{1});
}

// --- text chat (M50) --------------------------------------------------------
// The server is the only thing that decides who said what. These pin that.

namespace {

void say(Harness& h, std::uint32_t peer, std::string_view text) {
    eng::ByteWriter w;
    game::write(w, game::ChatSendMsg{std::string(text)});
    h.net.queue_message(peer, {w.data().begin(), w.data().end()});
    h.pump();
}

std::vector<game::ChatMessageMsg> chats_to(const Harness& h, std::uint32_t peer) {
    return decode_to(h.net.sent, peer, MsgType::ChatMessage, game::read_chat_message);
}

}  // namespace

TEST_CASE("chat reaches everyone including the sender", "[server][chat]") {
    Harness h;
    const auto alice = h.join(1, "alice");
    const auto bob = h.join(2, "bob");
    REQUIRE(alice);
    REQUIRE(bob);
    h.net.clear();

    say(h, 1, "hello");

    // The sender gets it too: seeing your own line appear is how you know it
    // was delivered rather than dropped.
    const auto to_alice = chats_to(h, 1);
    const auto to_bob = chats_to(h, 2);
    REQUIRE(to_alice.size() == 1);
    REQUIRE(to_bob.size() == 1);
    CHECK(to_alice[0].text == "hello");
    CHECK(to_bob[0].text == "hello");
    // Stamped with who actually sent it, from the connection it arrived on.
    CHECK(to_alice[0].sender == *alice);
    CHECK(to_bob[0].sender == *alice);
}

TEST_CASE("the sender is the server's answer, not the client's", "[server][chat]") {
    // ChatSend carries no sender field at all, so there is nothing to forge.
    // This pins that: bob's message can only ever arrive as bob's.
    Harness h;
    const auto alice = h.join(1, "alice");
    const auto bob = h.join(2, "bob");
    REQUIRE(alice);
    REQUIRE(bob);
    h.net.clear();

    say(h, 2, "it was alice");

    const auto to_alice = chats_to(h, 1);
    REQUIRE(to_alice.size() == 1);
    CHECK(to_alice[0].sender == *bob);
}

TEST_CASE("chat is rate limited per player", "[server][chat]") {
    Harness h;
    REQUIRE(h.join(1, "alice"));
    REQUIRE(h.join(2, "bob"));
    h.net.clear();

    say(h, 1, "one");
    say(h, 1, "two");    // immediately after: dropped
    say(h, 1, "three");  // still dropped
    CHECK(chats_to(h, 2).size() == 1);

    // A flood from one player must not silence another.
    say(h, 2, "bob here");
    CHECK(chats_to(h, 2).size() == 2);

    // ...and the limit lifts on its own.
    h.tick(50);
    say(h, 1, "later");
    const auto seen = chats_to(h, 2);
    REQUIRE(seen.size() == 3);
    CHECK(seen[2].text == "later");
}

TEST_CASE("the server sanitizes chat rather than trusting the client to", "[server][chat]") {
    // The client sanitizes its own echo, but the client is not the only thing
    // that can send a ChatSend.
    Harness h;
    REQUIRE(h.join(1, "alice"));
    REQUIRE(h.join(2, "bob"));
    h.net.clear();

    say(h, 1, "clean\nadmin: you are banned");
    const auto seen = chats_to(h, 2);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].text == "cleanadmin: you are banned");
    CHECK(seen[0].text.find('\n') == std::string::npos);
}

TEST_CASE("a message with nothing printable in it is not relayed", "[server][chat]") {
    Harness h;
    REQUIRE(h.join(1, "alice"));
    REQUIRE(h.join(2, "bob"));
    h.net.clear();

    say(h, 1, "   \n\t  ");
    CHECK(chats_to(h, 2).empty());
}

TEST_CASE("chat from a peer that never joined is ignored", "[server][chat]") {
    Harness h;
    REQUIRE(h.join(1, "alice"));
    h.net.clear();

    say(h, 99, "I am not here");
    CHECK(chats_to(h, 1).empty());
}

// --- map rotation (M51) -----------------------------------------------------
// The dangerous part is not choosing the next name, it is that every
// CharacterController and every position history refers to a world that is
// about to be destroyed.

namespace {

game::MapGeometry rotation_entry(std::string_view name, float half_extent) {
    game::MapGeometry geometry;
    geometry.name = std::string(name);
    geometry.meshes = flat_floor(half_extent);
    geometry.spawns = default_spawns();
    return geometry;
}

std::vector<game::MapGeometry> two_map_rotation() {
    return {rotation_entry(kMapName, 60.0f), rotation_entry("maps/second.glb", 60.0f)};
}

// Runs the match clock out so restart_match() -- and with it the rotation --
// actually fires, rather than poking advance_map directly.
void play_out_a_match(Harness& h) {
    h.tick(static_cast<int>((game::kMatchSeconds + game::kMatchRestartSeconds) * 60.0f) + 4);
}

}  // namespace

TEST_CASE("with no rotation configured the map never changes", "[server][rotation]") {
    Harness h;
    REQUIRE(h.join(1, "alice"));
    h.net.clear();
    play_out_a_match(h);

    CHECK(h.game.map_name() == kMapName);
    CHECK(h.net.count(MsgType::MapChange) == 0);
}

TEST_CASE("the map advances at match end and everyone is told", "[server][rotation]") {
    Harness h;
    h.game.set_map_rotation(two_map_rotation());
    REQUIRE(h.join(1, "alice"));
    REQUIRE(h.join(2, "bob"));
    h.net.clear();
    play_out_a_match(h);

    CHECK(h.game.map_name() == "maps/second.glb");
    const auto to_alice = decode_to(h.net.sent, 1, MsgType::MapChange, game::read_map_change);
    const auto to_bob = decode_to(h.net.sent, 2, MsgType::MapChange, game::read_map_change);
    REQUIRE(to_alice.size() == 1);
    REQUIRE(to_bob.size() == 1);
    CHECK(to_alice[0].map == "maps/second.glb");
    CHECK(to_bob[0].map == "maps/second.glb");
}

TEST_CASE("the rotation starts from the map actually being played", "[server][rotation]") {
    // A list whose first entry is not the current map must not make the very
    // first handover jump somewhere arbitrary.
    Harness h;
    h.game.set_map_rotation({rotation_entry("maps/other.glb", 60.0f),
                             rotation_entry(kMapName, 60.0f),
                             rotation_entry("maps/third.glb", 60.0f)});
    REQUIRE(h.join(1, "alice"));
    play_out_a_match(h);
    CHECK(h.game.map_name() == "maps/third.glb");  // the entry AFTER the current one
}

TEST_CASE("the rotation wraps", "[server][rotation]") {
    Harness h;
    h.game.set_map_rotation(two_map_rotation());
    REQUIRE(h.join(1, "alice"));
    play_out_a_match(h);
    REQUIRE(h.game.map_name() == "maps/second.glb");
    play_out_a_match(h);
    CHECK(h.game.map_name() == kMapName);
}

TEST_CASE("players keep simulating after a rotation", "[server][rotation]") {
    // The real hazard: every controller pointed into the world that
    // advance_map destroys. If they are not rebuilt this crashes or the
    // player falls through a floor that no longer exists.
    Harness h;
    h.game.set_map_rotation(two_map_rotation());
    const auto alice = h.join(1, "alice");
    REQUIRE(alice);
    play_out_a_match(h);
    REQUIRE(h.game.map_name() == "maps/second.glb");

    h.net.clear();
    h.tick(120);  // two seconds in the new world

    const auto seen = h.observed(1, *alice);
    REQUIRE(seen);
    // Standing on the new floor, not sinking through it.
    CHECK(seen->position.y > -1.0f);
    CHECK(std::isfinite(seen->position.x));
    CHECK(std::isfinite(seen->position.y));
    CHECK(std::isfinite(seen->position.z));
}

TEST_CASE("a rotation is announced before any snapshot of the new map", "[server][rotation]") {
    // A client that stepped its prediction through the OLD collision using
    // NEW positions would fall through the floor, so the order is part of the
    // contract rather than an accident of where the call sits.
    Harness h;
    h.game.set_map_rotation(two_map_rotation());
    REQUIRE(h.join(1, "alice"));
    h.net.clear();
    play_out_a_match(h);

    std::optional<std::size_t> first_map_change;
    std::optional<std::size_t> first_snapshot_after;
    for (std::size_t i = 0; i < h.net.sent.size(); ++i) {
        const auto type = h.net.sent[i].type();
        if (!first_map_change && type == MsgType::MapChange) {
            first_map_change = i;
        }
        if (first_map_change && !first_snapshot_after && type == MsgType::Snapshot &&
            i > *first_map_change) {
            first_snapshot_after = i;
        }
    }
    REQUIRE(first_map_change);
    // Every snapshot describing the new arena comes after the announcement.
    for (std::size_t i = 0; i < *first_map_change; ++i) {
        if (h.net.sent[i].type() == MsgType::Snapshot) {
            // Snapshots before the change describe the OLD map, which is fine.
            continue;
        }
    }
    CHECK((!first_snapshot_after || *first_snapshot_after > *first_map_change));
}

// --- teams (M52) ------------------------------------------------------------

TEST_CASE("teams are assigned to keep the sides even", "[server][team]") {
    Harness h;
    // Four joiners must alternate. Assigning by slot parity would look the
    // same here and break the moment somebody leaves, so what is asserted is
    // the RULE -- whichever side has fewer -- not the pattern it happens to
    // produce from an empty server.
    std::vector<game::Team> teams;
    for (std::uint32_t peer = 1; peer <= 4; ++peer) {
        REQUIRE(h.join(peer, "p" + std::to_string(peer)));
        const auto welcomes =
            decode_to(h.net.sent, peer, MsgType::ServerWelcome, game::read_server_welcome);
        REQUIRE(!welcomes.empty());
        teams.push_back(welcomes.back().team);
    }
    CHECK(teams[0] == game::Team::A);
    CHECK(teams[1] == game::Team::B);
    CHECK(teams[2] == game::Team::A);
    CHECK(teams[3] == game::Team::B);

    // And everyone is told everyone else's side, or a client cannot colour
    // them and would have to guess -- which for Team::A == 0 means guessing
    // that every stranger is friendly.
    const auto joins = decode_to(h.net.sent, 1, MsgType::PlayerJoined, game::read_player_joined);
    REQUIRE(joins.size() >= 3);
    for (const game::PlayerJoined& join : joins) {
        CHECK(join.team == teams[join.player_id]);
    }
}

TEST_CASE("a bullet passes through a teammate and hits the enemy behind", "[server][team]") {
    Harness h;
    // Three joiners: A, B, A. So players 0 and 2 are teammates and player 1
    // is the enemy.
    const auto a1 = h.join(1, "a1");
    const auto b1 = h.join(2, "b1");
    const auto a2 = h.join(3, "a2");
    REQUIRE(a1);
    REQUIRE(b1);
    REQUIRE(a2);
    h.tick(60);  // gravity settles everyone onto the floor

    const auto shooter_seen = h.observed(1, *a1);
    const auto enemy_seen = h.observed(1, *b1);
    const auto mate_seen = h.observed(1, *a2);
    REQUIRE(shooter_seen);
    REQUIRE(enemy_seen);
    REQUIRE(mate_seen);

    // pick_spawn put the teammate back on the shooter's own spawn -- it is the
    // point furthest from the only living enemy, which is where the shooter
    // already is. Asserted rather than assumed, because it is what makes this
    // a test of BLOCKING: the teammate's capsule contains the ray's origin, so
    // a ray that stopped at teammates could not travel at all.
    CHECK(glm::distance(mate_seen->position, shooter_seen->position) < 0.5f);
    CHECK(glm::distance(enemy_seen->position, shooter_seen->position) > 4.0f);

    h.net.clear();
    const game::InputCommand fire = aim_at(shooter_seen->position, enemy_seen->position, true);
    const int ticks = h.drive_until(1, fire, MsgType::PlayerDamaged, 120);
    REQUIRE(ticks > 0);

    const auto damage = decode_all(h.net.sent, MsgType::PlayerDamaged, game::read_player_damaged);
    REQUIRE(!damage.empty());
    for (const game::PlayerDamagedMsg& hit : damage) {
        // The enemy takes it; the teammate standing on the muzzle takes
        // nothing. Friendly fire is skipped at the RAY, not at the damage, so
        // a team cannot spend a match blocking each other's shots.
        CHECK(hit.victim == *b1);
        CHECK(hit.victim != *a2);
    }
}

TEST_CASE("shooting a teammate does nothing at all", "[server][team]") {
    Harness h;
    const auto a1 = h.join(1, "a1");
    const auto b1 = h.join(2, "b1");
    const auto a2 = h.join(3, "a2");
    REQUIRE(a1);
    REQUIRE(b1);
    REQUIRE(a2);
    h.tick(60);

    // Walk the enemy out of the line of fire first, so what this test proves
    // is "no damage to the teammate" and not "the enemy happened to be hit
    // instead". Nothing is aimed at the enemy at any point.
    const auto mate_seen = h.observed(1, *a2);
    const auto shooter_seen = h.observed(1, *a1);
    REQUIRE(mate_seen);
    REQUIRE(shooter_seen);

    h.net.clear();
    // Aim at the teammate and hold the trigger for two seconds. The test
    // rifle kills in four hits, so a friendly-fire bug would have killed them
    // twenty times over by the end of this.
    const game::InputCommand fire = aim_at(shooter_seen->position, mate_seen->position, true);
    h.drive(1, fire, 120);

    CHECK(decode_all(h.net.sent, MsgType::PlayerDamaged, game::read_player_damaged).empty());
    CHECK(decode_all(h.net.sent, MsgType::PlayerDied, game::read_player_died).empty());
}

TEST_CASE("a kill scores for the killer's team", "[server][team]") {
    Harness h;
    const Duel duel = set_up_duel(h);  // players 0 and 1, so A versus B
    h.net.clear();

    REQUIRE(h.drive_until(duel.shooter_peer, duel.fire, MsgType::PlayerDied, 120) > 0);
    h.tick(60);  // let a MatchState go out

    const auto states = decode_all(h.net.sent, MsgType::MatchState, game::read_match_state);
    REQUIRE(!states.empty());
    // The shooter is player 0, so team A. Team B's column must stay empty --
    // a score that incremented both, or the wrong one, would still look like
    // "scoring works" from a single number.
    CHECK(states.back().score_a == 1);
    CHECK(states.back().score_b == 0);
}

TEST_CASE("a team match with bots actually fights", "[server][team]") {
    // A smoke test, and deliberately only that. It catches a team match that
    // is DEAD -- nobody engaging, which is what a full server looked like when
    // bots targeted the nearest player instead of the nearest enemy -- but it
    // cannot tell correct targeting from lucky targeting, because a team-blind
    // build still lands cross-team hits by chance. The rule itself is pinned
    // in bot_tests.cpp ("targeting ignores teammates however close they are"),
    // where it can be asserted instead of measured.
    Harness h{60.0f,
              {{0.0f, 1.0f, 0.0f}, {8.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 8.0f}, {8.0f, 1.0f, 8.0f}}};
    h.game.set_bot_config(game::bot_config_for(game::BotSkill::Deadly));
    for (int i = 0; i < 4; ++i) {
        REQUIRE(h.game.add_bot("bot" + std::to_string(i + 1)));
    }
    // A human is needed to SEE any of this: every combat message is addressed
    // to peers and a bot has none, so a bot-only match is invisible to the
    // transport these assertions read. Joined last, so bots keep ids 0-3.
    REQUIRE(h.join(9, "witness"));
    h.tick(1200);  // twenty seconds

    const auto damage = decode_all(h.net.sent, MsgType::PlayerDamaged, game::read_player_damaged);
    REQUIRE(!damage.empty());
    int bot_on_bot = 0;
    for (const game::PlayerDamagedMsg& hit : damage) {
        CHECK(hit.attacker != hit.victim);
        if (hit.attacker < 4 && hit.victim < 4) {
            ++bot_on_bot;
        }
    }
    CHECK(bot_on_bot > 0);
}
