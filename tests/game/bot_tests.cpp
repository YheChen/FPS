#include "game/shared/bot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/shared/grenade.h"
#include "game/shared/hitscan.h"
#include "game/shared/nav.h"
#include "game/shared/player_movement.h"
#include "game/shared/weapon.h"
#include "tests/game/boxes.h"

namespace {

using Catch::Approx;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDt = 1.0f / 60.0f;

game::BotSenses looking_at_enemy(float distance) {
    game::BotSenses senses;
    senses.position = {0.0f, 0.0f, 0.0f};
    senses.yaw = 0.0f;
    senses.on_ground = true;
    senses.has_target = true;
    senses.target_visible = true;
    // Straight ahead is -Z at yaw 0.
    senses.target_position = {0.0f, 0.0f, -distance};
    return senses;
}

// Runs `ticks` of decisions, returning the last command.
game::InputCommand run(game::BotState& state, const game::BotSenses& senses,
                       const game::BotConfig& config, int ticks, std::uint32_t seed_base = 0) {
    game::InputCommand command;
    for (int i = 0; i < ticks; ++i) {
        command =
            game::decide(state, senses, config, kDt, seed_base + static_cast<std::uint32_t>(i));
    }
    return command;
}

TEST_CASE("yaw_towards matches the view direction convention", "[bot]") {
    // yaw 0 looks down -Z; positive yaw turns toward +X. If this disagrees
    // with view_direction(), bots aim at a mirror image of their target.
    CHECK(game::yaw_towards({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}) == Approx(0.0f));
    CHECK(game::yaw_towards({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}) == Approx(kPi * 0.5f));
    CHECK(game::yaw_towards({0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}) == Approx(-kPi * 0.5f));

    // Cross-check against the real thing rather than trusting the algebra.
    for (const float yaw : {0.0f, 0.7f, -1.4f, 2.9f, -3.0f}) {
        const glm::vec3 forward = game::view_direction(yaw, 0.0f);
        const float recovered = game::yaw_towards({0.0f, 0.0f, 0.0f}, forward);
        CHECK(std::abs(game::shortest_angle_delta(yaw, recovered)) < 1e-4f);
    }
}

TEST_CASE("shortest_angle_delta always takes the short way round", "[bot]") {
    CHECK(game::shortest_angle_delta(0.0f, 0.5f) == Approx(0.5f));
    CHECK(game::shortest_angle_delta(0.5f, 0.0f) == Approx(-0.5f));

    // Across the wrap: from just under +pi to just over -pi is a small step
    // forward, not a nearly-full turn back.
    const float delta = game::shortest_angle_delta(kPi - 0.1f, -kPi + 0.1f);
    CHECK(delta == Approx(0.2f).margin(1e-4f));
    CHECK(std::abs(delta) < kPi);

    // Never leaves (-pi, pi], whatever it is handed.
    for (int i = -20; i <= 20; ++i) {
        for (int j = -20; j <= 20; ++j) {
            const float d = game::shortest_angle_delta(static_cast<float>(i) * 0.5f,
                                                       static_cast<float>(j) * 0.5f);
            CHECK(d > -kPi - 1e-4f);
            CHECK(d <= kPi + 1e-4f);
        }
    }
}

TEST_CASE("a bot turns toward its target at a bounded rate", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    // Target 90 degrees to the right.
    game::BotSenses senses = looking_at_enemy(15.0f);
    senses.target_position = {15.0f, 0.0f, 0.0f};

    // Past the reaction delay, but only just.
    const int reaction_ticks = static_cast<int>(config.reaction_seconds / kDt) + 1;
    run(state, senses, config, reaction_ticks);
    const float before = state.aim_yaw;
    const game::InputCommand step = game::decide(state, senses, config, kDt, 999);
    // One tick must not snap all the way round: instant aim would make bots
    // unbeatable and would bypass the fire cone entirely.
    CHECK(std::abs(game::shortest_angle_delta(before, step.yaw)) <=
          config.turn_speed * kDt + 1e-5f);
    CHECK(std::abs(game::shortest_angle_delta(before, step.yaw)) > 0.0f);

    // Given time it arrives -- but only to within its aim error, never onto
    // the target exactly. That residual is the point of the whole mechanism.
    run(state, senses, config, 240);
    const float settled_error = std::abs(game::shortest_angle_delta(state.aim_yaw, kPi * 0.5f));
    CHECK(settled_error < config.aim_error_radians + 0.02f);
}

TEST_CASE("a bot holds fire until it is actually on target", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    game::BotSenses senses = looking_at_enemy(15.0f);
    senses.target_position = {15.0f, 0.0f, 0.0f};  // 90 degrees away

    const game::InputCommand first = game::decide(state, senses, config, kDt, 1);
    CHECK_FALSE(game::has_button(first, game::Button::Fire));

    // It gets there eventually. Checked over a window rather than on one
    // tick, because trigger discipline means a settled bot is deliberately
    // not firing much of the time.
    run(state, senses, config, 240);
    int firing = 0;
    for (int i = 0; i < 240; ++i) {
        if (game::has_button(
                game::decide(state, senses, config, kDt, 5000u + static_cast<std::uint32_t>(i)),
                game::Button::Fire)) {
            ++firing;
        }
    }
    CHECK(firing > 0);
}

// Reaction time is the most human-feeling of the three limits and the one a
// player notices most: it is the difference between rounding a corner and
// dying, and rounding a corner and getting to act first.
TEST_CASE("a bot does not react to a target instantly", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    const game::BotSenses senses = looking_at_enemy(15.0f);

    const int reaction_ticks = static_cast<int>(config.reaction_seconds / kDt);
    REQUIRE(reaction_ticks > 1);

    // Nothing at all for the whole delay: no shooting, and no turning either.
    const float initial_yaw = state.aim_yaw;
    for (int i = 0; i < reaction_ticks - 1; ++i) {
        const game::InputCommand command =
            game::decide(state, senses, config, kDt, static_cast<std::uint32_t>(i));
        CHECK_FALSE(game::has_button(command, game::Button::Fire));
    }
    CHECK(state.aim_yaw == Approx(initial_yaw));

    // And then it acts.
    int firing = 0;
    for (int i = 0; i < 240; ++i) {
        if (game::has_button(
                game::decide(state, senses, config, kDt, 900u + static_cast<std::uint32_t>(i)),
                game::Button::Fire)) {
            ++firing;
        }
    }
    CHECK(firing > 0);
}

// The bug this guards: aim used to converge on the target and stay there
// exactly, so `turn_speed` bounded how fast a bot got on target and NOTHING
// bounded how well it held. Steady-state error was zero -- an opponent that
// cannot be outplayed, only outranged.
TEST_CASE("a bot's aim never settles exactly on its target", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    const game::BotSenses senses = looking_at_enemy(15.0f);  // dead ahead

    run(state, senses, config, 240);  // long past reaction and acquisition

    float peak = 0.0f;
    for (int i = 0; i < 600; ++i) {  // ten seconds, several sway cycles
        game::decide(state, senses, config, kDt, 7000u + static_cast<std::uint32_t>(i));
        peak = std::max(peak, std::abs(game::shortest_angle_delta(state.aim_yaw, 0.0f)));
    }
    // It genuinely wanders...
    CHECK(peak > 0.3f * config.aim_error_radians);
    // ...but stays bounded, so a bot never spins off somewhere absurd.
    CHECK(peak <= config.aim_error_radians + 1e-3f);
}

// Trigger discipline is what actually decides lethality: the rifle is 600 rpm
// for 25 damage against 100 health, so a held trigger kills in 0.4 s of hits
// and aim error only changes how long "a moment" lasts.
TEST_CASE("a bot releases the trigger between bursts", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    const game::BotSenses senses = looking_at_enemy(12.0f);  // dead ahead, in range

    run(state, senses, config, 240);

    int firing = 0;
    int holding = 0;
    for (int i = 0; i < 600; ++i) {
        if (game::has_button(
                game::decide(state, senses, config, kDt, 3000u + static_cast<std::uint32_t>(i)),
                game::Button::Fire)) {
            ++firing;
        } else {
            ++holding;
        }
    }
    // Both happen: a bot that never fires is broken, and one that never stops
    // is the thing being fixed.
    CHECK(firing > 0);
    CHECK(holding > 0);
    // And the gaps are a real share of the time, not a single dropped tick.
    CHECK(holding > 60);
}

// A preset ladder is only useful if it is ordered. This is cheap insurance
// against a future edit that makes `easy` the hardest by accident.
TEST_CASE("the bot skill ladder is monotonic", "[bot]") {
    const auto easy = game::bot_config_for(game::BotSkill::Easy);
    const auto normal = game::bot_config_for(game::BotSkill::Normal);
    const auto hard = game::bot_config_for(game::BotSkill::Hard);
    const auto deadly = game::bot_config_for(game::BotSkill::Deadly);

    // Slower to react, the easier it is.
    CHECK(easy.reaction_seconds > normal.reaction_seconds);
    CHECK(normal.reaction_seconds > hard.reaction_seconds);
    CHECK(hard.reaction_seconds > deadly.reaction_seconds);
    // Worse aim, the easier it is.
    CHECK(easy.aim_error_radians > normal.aim_error_radians);
    CHECK(normal.aim_error_radians > hard.aim_error_radians);
    CHECK(hard.aim_error_radians > deadly.aim_error_radians);
    // More time off the trigger, the easier it is.
    CHECK(easy.burst_pause_seconds > normal.burst_pause_seconds);
    CHECK(normal.burst_pause_seconds > hard.burst_pause_seconds);
    CHECK(hard.burst_pause_seconds > deadly.burst_pause_seconds);

    // Deadly is the control case: the exact pre-M31 behaviour, kept so a
    // change to movement or hit detection can be measured against something
    // with no deliberate handicap in it.
    CHECK(deadly.reaction_seconds == 0.0f);
    CHECK(deadly.aim_error_radians == 0.0f);

    CHECK(game::bot_skill_from_name("easy") == game::BotSkill::Easy);
    CHECK(game::bot_skill_from_name("deadly") == game::BotSkill::Deadly);
    CHECK_FALSE(game::bot_skill_from_name("nonsense").has_value());
    CHECK(std::string_view{game::bot_skill_name(game::BotSkill::Hard)} == "hard");
}

TEST_CASE("a bot does not shoot through cover", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    game::BotSenses senses = looking_at_enemy(12.0f);
    senses.target_visible = false;  // known position, but behind a wall

    const game::InputCommand command = run(state, senses, config, 120);
    CHECK_FALSE(game::has_button(command, game::Button::Fire));
    // It still manoeuvres rather than freezing.
    CHECK(command.buttons != 0);
}

TEST_CASE("a bot ignores targets beyond its sight range", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    const game::BotSenses senses = looking_at_enemy(config.sight_range + 10.0f);

    const game::InputCommand command = run(state, senses, config, 60);
    CHECK_FALSE(game::has_button(command, game::Button::Fire));
}

TEST_CASE("a bot manages its range", "[bot]") {
    game::BotConfig config;

    // Too far: close the distance, sprinting.
    {
        game::BotState state;
        const game::BotSenses far = looking_at_enemy(config.engage_range + 8.0f);
        const game::InputCommand command = run(state, far, config, 10);
        CHECK(game::has_button(command, game::Button::Forward));
        CHECK(game::has_button(command, game::Button::Sprint));
        CHECK_FALSE(game::has_button(command, game::Button::Back));
    }
    // Too close: back off.
    {
        game::BotState state;
        const game::BotSenses close = looking_at_enemy(config.preferred_range - 5.0f);
        const game::InputCommand command = run(state, close, config, 10);
        CHECK(game::has_button(command, game::Button::Back));
        CHECK_FALSE(game::has_button(command, game::Button::Forward));
    }
    // In the pocket: hold range and strafe.
    {
        game::BotState state;
        const game::BotSenses mid =
            looking_at_enemy((config.preferred_range + config.engage_range) * 0.5f);
        const game::InputCommand command = run(state, mid, config, 10);
        CHECK_FALSE(game::has_button(command, game::Button::Forward));
        CHECK_FALSE(game::has_button(command, game::Button::Back));
        CHECK((game::has_button(command, game::Button::Left) ||
               game::has_button(command, game::Button::Right)));
    }
}

TEST_CASE("a bot strafes both ways over time", "[bot]") {
    // A bot that always strafes the same way circles predictably and, worse,
    // suggests the RNG is not being consumed at all.
    game::BotConfig config;
    game::BotState state;
    const game::BotSenses senses = looking_at_enemy(15.0f);

    bool saw_left = false;
    bool saw_right = false;
    for (int i = 0; i < 2000; ++i) {
        const game::InputCommand command =
            game::decide(state, senses, config, kDt, static_cast<std::uint32_t>(i));
        saw_left = saw_left || game::has_button(command, game::Button::Left);
        saw_right = saw_right || game::has_button(command, game::Button::Right);
    }
    CHECK(saw_left);
    CHECK(saw_right);
}

TEST_CASE("a bot backs away from a wall instead of grinding into it", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    game::BotSenses senses;
    senses.on_ground = true;
    senses.has_target = false;
    senses.forward_clearance = 0.5f;  // nose against a wall

    const game::InputCommand command = run(state, senses, config, 5);
    CHECK_FALSE(game::has_button(command, game::Button::Forward));
    CHECK(game::has_button(command, game::Button::Back));
}

TEST_CASE("wall avoidance overrides an engagement", "[bot]") {
    // Backing into a corner while chasing someone is the most visible way
    // this can look broken, so the wall check runs last and wins.
    game::BotConfig config;
    game::BotState state;
    game::BotSenses senses = looking_at_enemy(config.engage_range + 10.0f);
    senses.forward_clearance = 0.4f;

    const game::InputCommand command = run(state, senses, config, 5);
    CHECK_FALSE(game::has_button(command, game::Button::Forward));
    CHECK(game::has_button(command, game::Button::Back));
}

TEST_CASE("an idle bot wanders instead of standing still", "[bot]") {
    game::BotConfig config;
    game::BotState state;
    game::BotSenses senses;
    senses.on_ground = true;
    senses.has_target = false;

    const game::InputCommand command = run(state, senses, config, 30);
    CHECK(game::has_button(command, game::Button::Forward));
    CHECK_FALSE(game::has_button(command, game::Button::Fire));

    // And it changes heading over time rather than walking one line forever.
    const float first_heading = state.wander_yaw;
    run(state, senses, config, 600, 1000);
    CHECK(std::abs(game::shortest_angle_delta(first_heading, state.wander_yaw)) > 0.01f);
}

TEST_CASE("bot decisions are deterministic", "[bot]") {
    // Bots feed advance_player, so they are part of the simulation. If the
    // same seeds produced different commands, replays containing bots would
    // diverge -- which is exactly what M17's determinism guarantee forbids.
    game::BotConfig config;
    const game::BotSenses senses = looking_at_enemy(14.0f);

    game::BotState a;
    game::BotState b;
    for (std::uint32_t i = 0; i < 400; ++i) {
        const game::InputCommand ca = game::decide(a, senses, config, kDt, i);
        const game::InputCommand cb = game::decide(b, senses, config, kDt, i);
        REQUIRE(ca.buttons == cb.buttons);
        REQUIRE(ca.yaw == cb.yaw);
        REQUIRE(ca.pitch == cb.pitch);
        REQUIRE(ca.weapon_slot == cb.weapon_slot);
    }
}

TEST_CASE("bot commands are always well formed", "[bot]") {
    // Whatever it decides goes straight into advance_player and into replays,
    // so it must never be NaN or an illegal weapon slot.
    game::BotConfig config;
    game::BotState state;

    for (std::uint32_t i = 0; i < 500; ++i) {
        game::BotSenses senses;
        senses.on_ground = i % 3 == 0;
        senses.has_target = i % 2 == 0;
        senses.target_visible = i % 4 == 0;
        senses.position = {static_cast<float>(i % 7), 0.0f, static_cast<float>(i % 5)};
        senses.target_position = {static_cast<float>(i % 11) - 5.0f, 0.0f,
                                  static_cast<float>(i % 13) - 6.0f};
        senses.forward_clearance = static_cast<float>(i % 9) * 0.5f;

        const game::InputCommand command = game::decide(state, senses, config, kDt, i);
        CHECK(std::isfinite(command.yaw));
        CHECK(std::isfinite(command.pitch));
        CHECK(std::abs(command.pitch) <= 1.6f);
        CHECK(command.weapon_slot < game::kMaxWeapons);
    }
}

TEST_CASE("a bot on top of its target does not produce NaN aim", "[bot]") {
    // Exactly coincident positions make the horizontal distance zero, which
    // is where an atan2 or a normalize would blow up.
    game::BotConfig config;
    game::BotState state;
    game::BotSenses senses;
    senses.has_target = true;
    senses.target_visible = true;
    senses.position = {3.0f, 0.0f, 3.0f};
    senses.target_position = {3.0f, 0.0f, 3.0f};

    const game::InputCommand command = run(state, senses, config, 10);
    CHECK(std::isfinite(command.yaw));
    CHECK(std::isfinite(command.pitch));
}

TEST_CASE("targeting ignores teammates however close they are", "[bot][team]") {
    // The rule that was wrong for a whole milestone: the server took the
    // nearest living PLAYER while its own comment called it the nearest
    // "enemy". With one side those were the same sentence.
    //
    // Tested here rather than through a match because a match CANNOT tell the
    // two apart. A team-blind build still lands plenty of cross-team hits by
    // chance: measured damage counts came out 17 vs 16 over three seconds and
    // 79 vs 52 over twenty, and no threshold in between is a fact about the
    // rule rather than about this machine.
    const glm::vec3 me{0.0f, 0.0f, 0.0f};

    // A teammate at arm's length and an enemy across the map. The teammate is
    // a hundred times nearer, which is the point: a rule that merely PREFERRED
    // enemies would still pick the teammate here.
    const std::array<game::BotTarget, 2> mixed{{
        {{0.5f, 0.0f, 0.0f}, game::Team::A, true},
        {{50.0f, 0.0f, 0.0f}, game::Team::B, true},
    }};
    const auto picked = game::nearest_enemy(me, game::Team::A, mixed);
    REQUIRE(picked.has_value());
    CHECK(*picked == 1);

    // From the other side, the near one IS the enemy.
    const auto from_b = game::nearest_enemy(me, game::Team::B, mixed);
    REQUIRE(from_b.has_value());
    CHECK(*from_b == 0);

    // Nothing but teammates means NO target -- not "the closest teammate". A
    // bot holding a target it cannot hurt stands there shooting forever, which
    // is precisely what a full server looked like.
    const std::array<game::BotTarget, 2> friends{{
        {{0.5f, 0.0f, 0.0f}, game::Team::A, true},
        {{2.0f, 0.0f, 0.0f}, game::Team::A, true},
    }};
    CHECK_FALSE(game::nearest_enemy(me, game::Team::A, friends).has_value());

    // A dead enemy is not a target either, so the nearest LIVING one wins.
    const std::array<game::BotTarget, 2> corpse{{
        {{1.0f, 0.0f, 0.0f}, game::Team::B, false},
        {{9.0f, 0.0f, 0.0f}, game::Team::B, true},
    }};
    const auto living = game::nearest_enemy(me, game::Team::A, corpse);
    REQUIRE(living.has_value());
    CHECK(*living == 1);

    CHECK_FALSE(game::nearest_enemy(me, game::Team::A, {}).has_value());
}

TEST_CASE("bots carry the weapon the config names", "[bot]") {
    // Bots were pinned to slot 0, so no automated run could put the smg,
    // shotgun, sniper or knife into a real match -- verifying anything about
    // them meant editing a .cfg and remembering to revert it. The slot rides
    // every command a bot emits, so it has to survive whatever the bot is
    // doing that tick, not just the first one.
    game::BotConfig config = game::bot_config_for(game::BotSkill::Normal);
    config.weapon_slot = 3;

    game::BotState state;
    const game::BotSenses engaging = looking_at_enemy(15.0f);
    CHECK(game::decide(state, engaging, config, kDt, 0).weapon_slot == 3);
    CHECK(run(state, engaging, config, 120).weapon_slot == 3);

    // Idle, with nothing to shoot at, is a different path through decide().
    game::BotState idle_state;
    game::BotSenses idle;
    idle.position = {0.0f, 0.0f, 0.0f};
    idle.on_ground = true;
    idle.has_target = false;
    CHECK(run(idle_state, idle, config, 120).weapon_slot == 3);

    // And the default is still the rifle, so an unset flag changes nothing.
    game::BotConfig fallback = game::bot_config_for(game::BotSkill::Normal);
    game::BotState fallback_state;
    CHECK(run(fallback_state, engaging, fallback, 60).weapon_slot == 0);
}

TEST_CASE("bots throw grenades, and do not always cook them", "[bot][grenade]") {
    // Two things are under test, and the second is the interesting one: a bot
    // that ALWAYS cooks is one behaviour repeated, and what makes a fuse you
    // control interesting is that the timing varies.
    game::BotConfig config = game::bot_config_for(game::BotSkill::Deadly);

    // Runs one bot until it commits to a throw, and returns how long it held
    // the button: the cook, measured the same way the server measures it.
    const auto cook_seconds_for = [&](std::uint32_t seed_base) -> std::optional<float> {
        game::BotState state;
        game::BotSenses senses = looking_at_enemy(8.0f);
        senses.has_grenade = true;
        bool held_before = false;
        float held_for = 0.0f;
        for (int i = 0; i < 600; ++i) {
            const game::InputCommand command =
                game::decide(state, senses, config, kDt, seed_base + static_cast<std::uint32_t>(i));
            const bool held = game::has_button(command, game::Button::Grenade);
            if (held) {
                held_before = true;
                held_for += kDt;
            } else if (held_before) {
                return held_for;  // released: that is the throw
            }
        }
        return std::nullopt;
    };

    std::vector<float> cooks;
    for (std::uint32_t seed = 0; seed < 40; ++seed) {
        if (const auto held = cook_seconds_for(seed * 7919u)) {
            cooks.push_back(*held);
        }
    }

    // It throws at all.
    REQUIRE(cooks.size() >= 20u);

    // An UNCOOKED throw is held only for the two ticks the server needs to see
    // the button go down and come back up. A cooked one is held for a rolled
    // duration on top of that.
    const auto uncooked =
        std::count_if(cooks.begin(), cooks.end(), [](float c) { return c <= 2.5f * kDt; });
    const auto cooked = std::count_if(cooks.begin(), cooks.end(), [](float c) { return c > 0.2f; });

    // BOTH have to be COMMON, and "common" is the load-bearing word. An
    // earlier version asserted only `uncooked > 0`, and a bot rigged to cook
    // every single throw still passed it -- a duration rolled uniformly is
    // occasionally near zero anyway. A fraction cannot be satisfied by an
    // accident of rounding, and it is what actually says "not always".
    CHECK(uncooked * 4 >= static_cast<int>(cooks.size()));
    CHECK(cooked * 4 >= static_cast<int>(cooks.size()));

    // And the cooked ones VARY rather than all landing on the cap, which is
    // what a per-throw roll buys over a constant.
    const float longest = *std::max_element(cooks.begin(), cooks.end());
    const float shortest = *std::min_element(cooks.begin(), cooks.end());
    CHECK(longest - shortest > 0.2f);

    // Never near the fuse. A bot that kills itself is funny once and then
    // hands over a free kill.
    CHECK(longest < game::kGrenadeFuseSeconds * 0.6f);
}

TEST_CASE("a bot with no grenade never pulls a pin", "[bot][grenade]") {
    // Without this the bot commits to a throw, holds the button, and waits
    // forever for a grenade the server already refused to give it -- which
    // would also stop it shooting, since the two are exclusive.
    game::BotConfig config = game::bot_config_for(game::BotSkill::Deadly);
    game::BotState state;
    game::BotSenses senses = looking_at_enemy(8.0f);
    senses.has_grenade = false;

    bool ever_held = false;
    for (int i = 0; i < 600; ++i) {
        const game::InputCommand command =
            game::decide(state, senses, config, kDt, static_cast<std::uint32_t>(i));
        ever_held = ever_held || game::has_button(command, game::Button::Grenade);
    }
    CHECK_FALSE(ever_held);
}

TEST_CASE("bots do not throw grenades at point blank", "[bot][grenade]") {
    // At two metres the blast covers the thrower: the radius is 5.5 m.
    game::BotConfig config = game::bot_config_for(game::BotSkill::Deadly);
    game::BotState state;
    game::BotSenses senses = looking_at_enemy(2.0f);
    senses.has_grenade = true;

    bool ever_held = false;
    for (int i = 0; i < 600; ++i) {
        const game::InputCommand command =
            game::decide(state, senses, config, kDt, static_cast<std::uint32_t>(i));
        ever_held = ever_held || game::has_button(command, game::Button::Grenade);
    }
    CHECK_FALSE(ever_held);
}

// --- routes (M58) ------------------------------------------------------------

float horizontal(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2{a.x - b.x, a.z - b.z});
}

// A floor with a 16 m wall across the middle of it, x = 0, z in [-8, 8].
game::NavGrid walled_grid() {
    auto meshes = test::floor(15.0f);
    meshes.push_back(test::box({0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 16.0f}));
    return game::build_nav_grid(meshes);
}

TEST_CASE("a route with nothing to shoot at is walked facing where it goes", "[bot][route]") {
    game::BotSenses senses;
    senses.on_ground = true;
    senses.has_waypoint = true;
    senses.waypoint = {10.0f, 0.0f, 0.0f};  // due +X: a quarter turn from yaw 0
    game::BotState state;
    const game::BotConfig config;

    const game::InputCommand command = run(state, senses, config, 120);
    CHECK(std::abs(game::shortest_angle_delta(command.yaw, kPi * 0.5f)) < 0.05f);
    CHECK(game::has_button(command, game::Button::Forward));
    CHECK_FALSE(game::has_button(command, game::Button::Back));
    CHECK_FALSE(game::has_button(command, game::Button::Left));
    CHECK_FALSE(game::has_button(command, game::Button::Right));

    // And keeps facing it. The idle heading change fires every 2.8 s with no
    // route; over a route it must never fire, or the bot glances at a wall
    // mid-leg and walks that way.
    for (int i = 0; i < 600; ++i) {
        const game::InputCommand later =
            game::decide(state, senses, config, kDt, 1000u + static_cast<std::uint32_t>(i));
        REQUIRE(std::abs(game::shortest_angle_delta(later.yaw, kPi * 0.5f)) < 0.05f);
    }
}

TEST_CASE("a route while hunting an unseen enemy moves the feet, not the gaze", "[bot][route]") {
    // Enemy straight ahead behind cover, 20 m off: not a fight, so the route
    // decides where the feet go while the gaze stays on where the enemy is.
    // Deadly, so no reaction delay muddies which way the bot is looking.
    const game::BotConfig config = game::bot_config_for(game::BotSkill::Deadly);
    game::BotSenses senses = looking_at_enemy(20.0f);
    senses.target_visible = false;
    senses.has_waypoint = true;

    SECTION("a waypoint to the right is strafed to") {
        senses.waypoint = {5.0f, 0.0f, 0.0f};
        game::BotState state;
        const game::InputCommand command = run(state, senses, config, 30);
        CHECK(command.yaw == Approx(0.0f).margin(1e-4f));
        CHECK(game::has_button(command, game::Button::Right));
        CHECK_FALSE(game::has_button(command, game::Button::Left));
        CHECK_FALSE(game::has_button(command, game::Button::Forward));
        CHECK_FALSE(game::has_button(command, game::Button::Back));
        CHECK_FALSE(game::has_button(command, game::Button::Sprint));
        CHECK_FALSE(game::has_button(command, game::Button::Fire));
    }
    SECTION("a waypoint behind is backed toward") {
        senses.waypoint = {0.0f, 0.0f, 5.0f};
        game::BotState state;
        const game::InputCommand command = run(state, senses, config, 30);
        CHECK(command.yaw == Approx(0.0f).margin(1e-4f));
        CHECK(game::has_button(command, game::Button::Back));
        CHECK_FALSE(game::has_button(command, game::Button::Forward));
        CHECK_FALSE(game::has_button(command, game::Button::Sprint));
    }
    SECTION("a waypoint ahead and off to one side is run at") {
        senses.waypoint = {-3.0f, 0.0f, -5.0f};  // ~31 degrees left of the gaze
        game::BotState state;
        const game::InputCommand command = run(state, senses, config, 30);
        CHECK(game::has_button(command, game::Button::Forward));
        CHECK(game::has_button(command, game::Button::Left));
        CHECK_FALSE(game::has_button(command, game::Button::Right));
        CHECK(game::has_button(command, game::Button::Sprint));
    }
}

TEST_CASE("a visible enemy in range is fought, whatever the route says", "[bot][route]") {
    game::BotSenses senses = looking_at_enemy(15.0f);  // between preferred and engage range
    senses.has_waypoint = true;
    senses.waypoint = {0.0f, 0.0f, 10.0f};  // the route says: behind you
    game::BotState state;
    const game::InputCommand command =
        run(state, senses, game::bot_config_for(game::BotSkill::Deadly), 60);
    // Holding range: strafing, neither closing nor backing off, and certainly
    // not walking away from a target it can see.
    CHECK_FALSE(game::has_button(command, game::Button::Back));
    CHECK_FALSE(game::has_button(command, game::Button::Forward));
    CHECK((game::has_button(command, game::Button::Left) ||
           game::has_button(command, game::Button::Right)));
}

TEST_CASE("a wall ahead does not override a route", "[bot][route]") {
    game::BotSenses senses;
    senses.on_ground = true;
    senses.has_waypoint = true;
    senses.waypoint = {0.0f, 0.0f, -6.0f};  // straight ahead
    senses.forward_clearance = 1.0f;        // the ray sees a wall; the route knows better
    game::BotState state;
    const game::InputCommand command = run(state, senses, game::BotConfig{}, 10);
    CHECK(game::has_button(command, game::Button::Forward));
    CHECK_FALSE(game::has_button(command, game::Button::Back));

    // Steering blind, the same wall still turns the bot away.
    senses.has_waypoint = false;
    game::BotState blind;
    const game::InputCommand avoided = run(blind, senses, game::BotConfig{}, 10);
    CHECK(game::has_button(avoided, game::Button::Back));
    CHECK_FALSE(game::has_button(avoided, game::Button::Forward));
}

TEST_CASE("navigate routes a chase round a wall and keeps the route", "[bot][route]") {
    const game::NavGrid grid = walled_grid();
    game::BotState state;
    const game::BotConfig config;
    const glm::vec3 position{-5.0f, 0.0f, 0.0f};
    const glm::vec3 enemy{5.0f, 0.0f, 0.0f};

    const auto waypoint = game::navigate(state, grid, position, enemy, config, kDt, 1);
    REQUIRE(waypoint);
    // Not at the enemy -- that is through the wall -- but past one of its
    // ends, still on this side of it.
    CHECK(std::abs(waypoint->z) > 7.5f);
    CHECK(waypoint->x < 0.0f);
    CHECK_FALSE(state.route.roaming);
    CHECK(state.route.points.size() >= 2);

    // Next tick, the same answer from the same route: no search ran.
    const std::vector<glm::vec3> points = state.route.points;
    const auto again = game::navigate(state, grid, position, enemy, config, kDt, 2);
    REQUIRE(again);
    CHECK(*again == *waypoint);
    CHECK(state.route.points == points);

    // The enemy edges along: the route is kept until the timer runs out,
    // then refreshed toward where the enemy is now.
    const glm::vec3 edged{5.0f, 0.0f, 1.0f};
    int refreshed_at = -1;
    for (int tick = 0; tick < 60 && refreshed_at < 0; ++tick) {
        game::navigate(state, grid, position, edged, config, kDt,
                       10u + static_cast<std::uint32_t>(tick));
        if (horizontal(state.route.goal, edged) < 0.01f) {
            refreshed_at = tick;
        }
    }
    REQUIRE(refreshed_at >= 0);
    CHECK(refreshed_at > 20);   // not at once...
    CHECK(refreshed_at <= 31);  // ...but within the half-second repath interval

    // The enemy sprints off: that is a new chase, replanned at once.
    const glm::vec3 far{5.0f, 0.0f, 12.0f};
    game::navigate(state, grid, position, far, config, kDt, 99);
    CHECK(horizontal(state.route.goal, far) < 0.01f);
}

TEST_CASE("navigate walks the route down and says when it has arrived", "[bot][route]") {
    const game::NavGrid grid = game::build_nav_grid(test::floor(20.0f));
    game::BotState state;
    const game::BotConfig config;
    const glm::vec3 enemy{8.0f, 0.0f, 0.0f};

    const auto first = game::navigate(state, grid, {-8.0f, 0.0f, 0.0f}, enemy, config, kDt, 1);
    REQUIRE(first);
    CHECK(horizontal(*first, enemy) < grid.cell_size());  // open floor: one leg

    // Standing on it: the route is done, and the bot is told so.
    const auto arrived = game::navigate(state, grid, *first, enemy, config, kDt, 2);
    CHECK_FALSE(arrived);
    CHECK(state.route.next == state.route.points.size());
}

TEST_CASE("navigate roams somewhere reachable and far enough when idle", "[bot][route]") {
    const game::NavGrid grid = game::build_nav_grid(test::floor(30.0f));
    game::BotState state;
    const game::BotConfig config;
    const glm::vec3 position{0.0f, 0.0f, 0.0f};

    const auto waypoint = game::navigate(state, grid, position, std::nullopt, config, kDt, 7);
    REQUIRE(waypoint);
    CHECK(state.route.roaming);
    CHECK(horizontal(state.route.goal, position) >= config.route_roam_min_distance);
    const auto goal_cell = grid.cell_of(state.route.goal);
    REQUIRE(goal_cell);
    CHECK(grid.walkable(*goal_cell));

    // Arriving picks somewhere new, not the same place again.
    const glm::vec3 first_goal = state.route.goal;
    CHECK_FALSE(game::navigate(state, grid, first_goal, std::nullopt, config, kDt, 8));
    const auto next = game::navigate(state, grid, first_goal, std::nullopt, config, kDt, 9);
    REQUIRE(next);
    CHECK(horizontal(state.route.goal, first_goal) >= config.route_roam_min_distance);

    // An enemy appearing ends the roam at once.
    const glm::vec3 enemy{-20.0f, 0.0f, 5.0f};
    game::navigate(state, grid, first_goal, enemy, config, kDt, 10);
    CHECK_FALSE(state.route.roaming);
    CHECK(horizontal(state.route.goal, enemy) < 0.01f);
}

TEST_CASE("a bot that stops making progress drops its route", "[bot][route]") {
    const game::NavGrid grid = walled_grid();
    game::BotState state;
    const game::BotConfig config;
    const glm::vec3 position{-5.0f, 0.0f, 0.0f};
    const glm::vec3 enemy{5.0f, 0.0f, 0.0f};

    // Standing still with somewhere to go, for well under the limit: the
    // route stands -- including across the half-second replans, each of
    // which must NOT restart the clock.
    const int limit = static_cast<int>(std::lround(config.route_stuck_seconds / kDt));
    for (int tick = 0; tick < limit - 10; ++tick) {
        REQUIRE(game::navigate(state, grid, position, enemy, config, kDt,
                               static_cast<std::uint32_t>(tick)));
    }
    // ...and then, around the limit, it does not. Dropped outright, so a
    // fresh one is planned next tick instead of leaning on the same obstacle.
    int dropped_after = -1;
    for (int tick = 0; tick < 20 && dropped_after < 0; ++tick) {
        if (!game::navigate(state, grid, position, enemy, config, kDt,
                            500u + static_cast<std::uint32_t>(tick))) {
            dropped_after = tick;
        }
    }
    CHECK(dropped_after >= 0);
    CHECK(state.route.points.empty());

    // Moving resets the clock: a bot walking its route is never "stuck".
    game::BotState walking;
    glm::vec3 at = position;
    for (int tick = 0; tick < 2 * limit; ++tick) {
        at.z += 0.05f;  // 3 m/s along the wall
        REQUIRE(game::navigate(walking, grid, at, enemy, config, kDt,
                               900u + static_cast<std::uint32_t>(tick)));
    }
}

TEST_CASE("navigate on an empty grid has nothing to say", "[bot][route]") {
    const game::NavGrid grid;
    game::BotState state;
    state.route.points.push_back({1.0f, 0.0f, 1.0f});  // stale, from some other map
    CHECK_FALSE(game::navigate(state, grid, {0.0f, 0.0f, 0.0f}, glm::vec3{5.0f, 0.0f, 0.0f},
                               game::BotConfig{}, kDt, 1));
    CHECK(state.route.points.empty());
}

}  // namespace
