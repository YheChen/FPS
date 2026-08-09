#include "game/shared/bot.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/shared/hitscan.h"
#include "game/shared/player_movement.h"
#include "game/shared/weapon.h"

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

}  // namespace
