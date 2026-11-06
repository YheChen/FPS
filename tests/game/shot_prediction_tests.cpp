#include "game/shared/shot_prediction.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <deque>
#include <utility>

#include "game/shared/player_movement.h"

namespace {

game::Arsenal two_guns() {
    game::Arsenal arsenal;
    game::WeaponConfig rifle;
    rifle.name = "rifle";
    rifle.magazine_size = 30;
    rifle.rounds_per_minute = 600.0f;  // a shot every 6 ticks
    rifle.reload_seconds = 1.8f;
    rifle.switch_seconds = 0.4f;
    rifle.ads_fov_scale = 0.7f;  // has sights, so Aim moves ads_fraction
    arsenal.weapons.push_back(rifle);
    game::WeaponConfig shotgun;
    shotgun.name = "shotgun";
    shotgun.magazine_size = 6;
    shotgun.rounds_per_minute = 75.0f;
    shotgun.reload_seconds = 2.4f;
    shotgun.switch_seconds = 0.5f;
    shotgun.automatic = false;
    arsenal.weapons.push_back(shotgun);
    return arsenal;
}

game::PredictedShot shot(std::uint32_t sequence, std::uint32_t tick = 0) {
    return {sequence, tick, 0};
}

// Every field, compared exactly. A reconcile that replays the same inputs
// through the same arithmetic must reproduce the same bits, and "close
// enough" would hide a replay that skipped or doubled a tick.
void require_same(const game::Loadout& a, const game::Loadout& b) {
    REQUIRE(a.slot == b.slot);
    REQUIRE(a.switch_remaining_seconds == b.switch_remaining_seconds);
    REQUIRE(a.ads_fraction == b.ads_fraction);
    for (std::size_t i = 0; i < game::kMaxWeapons; ++i) {
        INFO("weapon " << i);
        REQUIRE(a.weapons[i].ammo == b.weapons[i].ammo);
        REQUIRE(a.weapons[i].cooldown_seconds == b.weapons[i].cooldown_seconds);
        REQUIRE(a.weapons[i].reload_remaining_seconds == b.weapons[i].reload_remaining_seconds);
        REQUIRE(a.weapons[i].trigger_was_held == b.weapons[i].trigger_was_held);
    }
}

// A deterministic script that exercises every rule the tick has: bursts of
// automatic fire, a semi-automatic pulled and released, a magazine run dry
// into an auto-reload, a manual reload, switches both ways, and the sights.
game::WeaponInput scripted_input(std::uint32_t sequence) {
    game::WeaponInput input;
    input.sequence = sequence;
    const std::uint32_t t = sequence % 600;
    input.weapon_slot = (t >= 200 && t < 400) ? 1 : 0;
    input.fire = (t % 50) < 30 && !(t >= 450 && t < 470);
    if (input.weapon_slot == 1) {
        input.fire = input.fire && (t % 4) < 2;  // semi-auto wants pulls
    }
    input.reload = t == 120 || t == 500;
    input.aim = (t / 37) % 2 == 0;
    return input;
}

// --- the ledger ---------------------------------------------------------------

TEST_CASE("an echo pairs with the prediction of the command it was fired on", "[shot_prediction]") {
    game::ShotLedger ledger;
    // Nothing predicted: a shot we did not see coming, which the caller must
    // draw in full rather than drop.
    auto c = ledger.confirm(5);
    CHECK_FALSE(c.shot.has_value());
    CHECK(c.refused == 0u);

    ledger.predict(shot(10));
    ledger.predict(shot(16));
    ledger.predict(shot(22));
    c = ledger.confirm(16);
    REQUIRE(c.shot.has_value());
    CHECK(c.shot->sequence == 16);
    // The server fired on 16 without first firing on 10. Echoes arrive in
    // the order the server fired, so 10's will never come.
    CHECK(c.refused == 1u);
    CHECK(ledger.pending() == 1u);

    c = ledger.confirm(22);
    REQUIRE(c.shot.has_value());
    CHECK(c.shot->sequence == 22);
    CHECK(c.refused == 0u);
    CHECK(ledger.pending() == 0u);
}

TEST_CASE("an echo nobody predicted does not steal the next prediction", "[shot_prediction]") {
    // THE bug the sequence numbers exist to fix. A server starved of input
    // re-runs the last command, and an automatic weapon can fire on it
    // twice. The first revision paired echoes by ORDER, so the second echo
    // of 100 took the prediction for 106; 106's real echo then matched
    // nothing and was drawn a second time -- and every shot after it was
    // off by one, silent when it fired and doubled when it was echoed.
    game::ShotLedger ledger;
    ledger.predict(shot(100));
    ledger.predict(shot(106));

    auto c = ledger.confirm(100);
    REQUIRE(c.shot.has_value());
    CHECK(c.shot->sequence == 100);

    c = ledger.confirm(100);  // the repeat
    CHECK_FALSE(c.shot.has_value());
    CHECK(ledger.pending() == 1u);

    c = ledger.confirm(106);
    REQUIRE(c.shot.has_value());
    CHECK(c.shot->sequence == 106);
    CHECK(c.refused == 0u);
}

TEST_CASE("an echo a command or two off is still the same trigger pull", "[shot_prediction]") {
    game::ShotLedger ledger;
    // A repeated command advanced the server's cooldown a tick the client
    // never ran, so it fired one command EARLY.
    ledger.predict(shot(107));
    auto c = ledger.confirm(106);
    REQUIRE(c.shot.has_value());
    CHECK(c.shot->sequence == 107);

    // And late, by as much as the tolerance.
    ledger.predict(shot(112));
    c = ledger.confirm(112 + game::ShotLedger::kPairingTolerance);
    REQUIRE(c.shot.has_value());
    CHECK(c.shot->sequence == 112);

    // One past it is a different shot: the old prediction was refused and
    // this echo is drawn in full.
    ledger.predict(shot(120));
    c = ledger.confirm(120 + game::ShotLedger::kPairingTolerance + 1);
    CHECK_FALSE(c.shot.has_value());
    CHECK(c.refused == 1u);
    CHECK(ledger.pending() == 0u);

    // A prediction too far AHEAD of the echo is left for its own echo.
    ledger.predict(shot(200));
    c = ledger.confirm(200 - game::ShotLedger::kPairingTolerance - 1);
    CHECK_FALSE(c.shot.has_value());
    CHECK(c.refused == 0u);
    CHECK(ledger.pending() == 1u);
}

TEST_CASE("a prediction nobody answers expires, and only that one", "[shot_prediction]") {
    game::ShotLedger ledger;
    ledger.predict(shot(10, /*tick=*/100));   // the server will refuse this one
    ledger.predict(shot(160, /*tick=*/250));  // still legitimately in flight

    // Not yet: a slow echo must never be discarded ahead of its arrival, or
    // it is then drawn twice.
    CHECK(ledger.expire(200, 180) == 0u);
    CHECK(ledger.pending() == 2u);

    CHECK(ledger.expire(300, 180) == 1u);
    CHECK(ledger.pending() == 1u);
    const auto survivor = ledger.confirm(160);
    REQUIRE(survivor.shot.has_value());
    CHECK(survivor.shot->tick == 250);

    // A tick counter that went backwards must not read as an enormous age:
    // unsigned subtraction did exactly that in the first version of this.
    ledger.predict(shot(300, /*tick=*/500));
    CHECK(ledger.expire(10, 180) == 0u);
    CHECK(ledger.pending() == 1u);
}

// --- the input history --------------------------------------------------------

TEST_CASE("the input history forgets what the server ran, and stays bounded", "[shot_prediction]") {
    game::WeaponInputHistory history;
    for (std::uint32_t s = 1; s <= 10; ++s) {
        history.record({s});
    }
    history.acknowledge(4);
    REQUIRE(history.size() == 6u);
    CHECK(history.inputs().front().sequence == 5);

    history.acknowledge(100);
    CHECK(history.size() == 0u);

    // A player who never fires, reloads or switches gets no statuses at all,
    // and must not grow this forever.
    for (std::uint32_t s = 1; s <= game::WeaponInputHistory::kCapacity + 10; ++s) {
        history.record({s});
    }
    CHECK(history.size() == game::WeaponInputHistory::kCapacity);
    CHECK(history.inputs().front().sequence == 11);
}

// --- reconciliation -----------------------------------------------------------

TEST_CASE("a status and its reconcile are inverses", "[shot_prediction]") {
    const game::Arsenal arsenal = two_guns();
    game::Loadout server;
    game::reset_loadout(server, arsenal);
    // Something in every field: the rifle mid-reload, the shotgun with a
    // spent shell and a cooldown, and the shotgun half raised.
    server.weapons[0].ammo = 0;
    server.weapons[0].reload_remaining_seconds = 0.75f;
    server.weapons[1].ammo = 4;
    server.weapons[1].cooldown_seconds = 0.3f;
    server.weapons[1].trigger_was_held = true;
    server.slot = 1;
    server.switch_remaining_seconds = 0.2f;

    game::Loadout client;
    game::reset_loadout(client, arsenal);
    game::reconcile_loadout(client, arsenal, game::describe_loadout(server, arsenal, 40),
                            game::WeaponInputHistory{});
    require_same(client, server);
}

TEST_CASE("reconciling a prediction that was right changes nothing", "[shot_prediction]") {
    // The property everything else rests on. The server and the client run
    // the same script; the server's statuses reach the client a round trip
    // late, by which time the client has run further. Snapping back to each
    // one and replaying must land on EXACTLY the live state, or every
    // status would nudge the HUD and the next predicted shot.
    const game::Arsenal arsenal = two_guns();
    game::Loadout server;
    game::Loadout client;
    game::reset_loadout(server, arsenal);
    game::reset_loadout(client, arsenal);
    game::WeaponInputHistory history;

    constexpr std::uint32_t kRoundTripTicks = 9;
    std::deque<std::pair<std::uint32_t, game::WeaponStatusMsg>> in_flight;  // arrival, status
    int reconciles = 0;
    int shots = 0;
    int reloads_started = 0;
    int reloads_finished = 0;
    int switches = 0;
    for (std::uint32_t seq = 1; seq <= 1500; ++seq) {
        const game::WeaponInput input = scripted_input(seq);
        history.record(input);
        game::update_loadout(client, arsenal, input.weapon_slot, input.fire, input.reload,
                             game::kTickSeconds, input.aim);

        const game::WeaponTickResult r =
            game::update_loadout(server, arsenal, input.weapon_slot, input.fire, input.reload,
                                 game::kTickSeconds, input.aim);
        shots += r.fired ? 1 : 0;
        reloads_started += r.reload_started ? 1 : 0;
        reloads_finished += r.reload_finished ? 1 : 0;
        switches += r.switched ? 1 : 0;
        // When the real server sends one.
        if (r.fired || r.reload_started || r.reload_finished || r.switched) {
            in_flight.emplace_back(seq + kRoundTripTicks,
                                   game::describe_loadout(server, arsenal, seq));
        }
        while (!in_flight.empty() && in_flight.front().first <= seq) {
            const game::Loadout before = client;
            history.acknowledge(in_flight.front().second.input_sequence);
            game::reconcile_loadout(client, arsenal, in_flight.front().second, history);
            INFO("status for seq " << in_flight.front().second.input_sequence << " at seq " << seq);
            require_same(client, before);
            in_flight.pop_front();
            ++reconciles;
        }
    }
    // The script has to have produced statuses of every kind for this to
    // mean anything.
    CHECK(reconciles > 50);
    CHECK(shots > 30);
    CHECK(reloads_started >= 2);
    CHECK(reloads_finished >= 2);
    CHECK(switches >= 4);
}

TEST_CASE("a shot the server refused is taken back, and later shots are kept",
          "[shot_prediction]") {
    const game::Arsenal arsenal = two_guns();
    game::Loadout client;
    game::reset_loadout(client, arsenal);
    game::WeaponInputHistory history;

    // The client fires on 10 and on 17. Seven ticks apart, not the rifle's
    // six: whether six ticks of float subtraction land exactly on zero is
    // not what this test is about.
    for (std::uint32_t seq = 1; seq <= 20; ++seq) {
        const game::WeaponInput input{seq, 0, seq == 10 || seq == 17, false, false};
        history.record(input);
        game::update_loadout(client, arsenal, 0, input.fire, false, game::kTickSeconds);
    }
    REQUIRE(client.weapons[0].ammo == 28);

    SECTION("the server fired on 10: its count is ours minus the shot still in flight") {
        game::Loadout server;
        game::reset_loadout(server, arsenal);
        for (std::uint32_t seq = 1; seq <= 12; ++seq) {
            game::update_loadout(server, arsenal, 0, seq == 10, false, game::kTickSeconds);
        }
        const game::Loadout live = client;
        game::reconcile_loadout(client, arsenal, game::describe_loadout(server, arsenal, 12),
                                history);
        require_same(client, live);
    }
    SECTION("the server refused 10: the round comes back, and 17 still counts") {
        // Say the server saw no trigger on 10 (the input was lost and the
        // starvation jump skipped it). Its loadout at 12 is a full magazine.
        game::Loadout server;
        game::reset_loadout(server, arsenal);
        game::reconcile_loadout(client, arsenal, game::describe_loadout(server, arsenal, 12),
                                history);
        CHECK(client.weapons[0].ammo == 29);
    }
}

TEST_CASE("a status from before a switch does not undo it", "[shot_prediction]") {
    // The status carries the slot, and for one sent before a local switch
    // that slot is the OLD weapon. The first revision of this wrote it
    // straight into the loadout and restarted the raise on every
    // fire-then-switch inside a round trip. Snap-and-replay re-runs the
    // switch from the command that made it, on the same tick.
    const game::Arsenal arsenal = two_guns();
    game::Loadout server;
    game::Loadout client;
    game::reset_loadout(server, arsenal);
    game::reset_loadout(client, arsenal);
    game::WeaponInputHistory history;
    for (std::uint32_t seq = 1; seq <= 12; ++seq) {
        const std::uint8_t slot = seq >= 8 ? 1 : 0;  // switch on 8
        const game::WeaponInput input{seq, slot, seq == 5, false, false};
        history.record(input);
        game::update_loadout(client, arsenal, slot, input.fire, false, game::kTickSeconds);
        if (seq <= 6) {
            game::update_loadout(server, arsenal, slot, input.fire, false, game::kTickSeconds);
        }
    }
    REQUIRE(client.slot == 1);
    REQUIRE(client.switch_remaining_seconds > 0.0f);
    const game::Loadout live = client;

    game::reconcile_loadout(client, arsenal, game::describe_loadout(server, arsenal, 6), history);
    require_same(client, live);
}

TEST_CASE("the sights survive a reconcile", "[shot_prediction]") {
    // ads_fraction is the FOV. The server's copy can differ from ours by a
    // tick whenever the two disagree about sprinting, and taking it would pop
    // the zoom; nothing else in the tick reads it, so ours is kept.
    const game::Arsenal arsenal = two_guns();
    game::Loadout client;
    game::reset_loadout(client, arsenal);
    client.ads_fraction = 0.6f;
    game::WeaponInputHistory history;
    history.record({5, 0, false, false, /*aim=*/false});  // replaying this lowers the sights

    game::Loadout server;
    game::reset_loadout(server, arsenal);
    game::reconcile_loadout(client, arsenal, game::describe_loadout(server, arsenal, 4), history);
    CHECK(client.ads_fraction == 0.6f);
}

TEST_CASE("a status only replays what came after it", "[shot_prediction]") {
    // The sequence is the whole contract: an input at or before it is
    // already inside the status, and replaying it again would fire a shot
    // twice. One that is not must be replayed, or it is lost.
    const game::Arsenal arsenal = two_guns();
    game::WeaponInputHistory history;
    for (std::uint32_t seq = 1; seq <= 10; ++seq) {
        history.record({seq, 0, /*fire=*/seq == 4 || seq == 10, false, false});
    }
    game::Loadout server;
    game::reset_loadout(server, arsenal);
    server.weapons[0].ammo = 29;  // fired on 4, by its own count

    game::Loadout client;
    game::reset_loadout(client, arsenal);
    const std::size_t replayed = game::reconcile_loadout(
        client, arsenal, game::describe_loadout(server, arsenal, 4), history);
    CHECK(replayed == 6u);
    CHECK(client.weapons[0].ammo == 28);  // 29, then the shot on 10
}

}  // namespace
