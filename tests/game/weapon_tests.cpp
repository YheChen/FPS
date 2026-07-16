#include "game/shared/weapon.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

constexpr float kTick = 1.0f / 60.0f;

game::WeaponConfig test_config() {
    game::WeaponConfig config;
    config.damage = 25.0f;
    config.rounds_per_minute = 600.0f;  // 0.1 s = 6 ticks between shots
    config.magazine_size = 5;
    config.reload_seconds = 0.5f;  // 30 ticks
    return config;
}

TEST_CASE("weapon config parses a full file", "[weapon]") {
    const auto config = game::parse_weapon_config(R"(# test weapon
name=blaster
damage=40
rounds_per_minute=300
magazine_size=8
reload_seconds=2.5
range=60
spread_degrees=1.5
)");
    REQUIRE(config.has_value());
    CHECK(config->name == "blaster");
    CHECK(config->damage == 40.0f);
    CHECK(config->rounds_per_minute == 300.0f);
    CHECK(config->magazine_size == 8);
    CHECK(config->reload_seconds == 2.5f);
    CHECK(config->range == 60.0f);
    CHECK(config->spread_degrees == 1.5f);
    CHECK(config->shot_interval_seconds() == Catch::Approx(0.2f));
}

TEST_CASE("weapon config rejects malformed values and bad ranges", "[weapon]") {
    CHECK_FALSE(game::parse_weapon_config("damage=abc").has_value());
    CHECK_FALSE(game::parse_weapon_config("no equals sign here").has_value());
    CHECK_FALSE(game::parse_weapon_config("magazine_size=0").has_value());
    CHECK_FALSE(game::parse_weapon_config("rounds_per_minute=-5").has_value());
    // Unknown keys are skipped, not fatal.
    CHECK(game::parse_weapon_config("shiny=true\ndamage=10").has_value());
}

TEST_CASE("fire rate is gated by the shot interval", "[weapon]") {
    const game::WeaponConfig config = test_config();
    game::WeaponState state;
    state.ammo = config.magazine_size;

    int shots = 0;
    for (int i = 0; i < 24; ++i) {  // 24 ticks = 0.4 s -> shots at ticks 0,6,12,18
        const auto result = game::update_weapon(state, config, true, false, kTick);
        if (result.fired) {
            ++shots;
        }
    }
    CHECK(shots == 4);
    CHECK(state.ammo == config.magazine_size - 4);
}

TEST_CASE("empty magazine dry-fires once and auto-reloads", "[weapon]") {
    const game::WeaponConfig config = test_config();
    game::WeaponState state;
    state.ammo = 0;

    const auto first = game::update_weapon(state, config, true, false, kTick);
    CHECK(first.dry_fired);
    CHECK(first.reload_started);
    CHECK_FALSE(first.fired);
    CHECK(state.reloading());

    // While reloading, holding fire does nothing.
    bool fired_or_dry = false;
    bool finished = false;
    for (int i = 0; i < 40; ++i) {
        const auto result = game::update_weapon(state, config, true, false, kTick);
        fired_or_dry |= result.dry_fired;
        finished |= result.reload_finished;
        if (result.reload_finished) {
            break;
        }
    }
    CHECK_FALSE(fired_or_dry);
    CHECK(finished);
    CHECK(state.ammo == config.magazine_size);
}

TEST_CASE("manual reload only with a non-full magazine", "[weapon]") {
    const game::WeaponConfig config = test_config();
    game::WeaponState state;
    state.ammo = config.magazine_size;

    // Full mag: reload request ignored.
    auto result = game::update_weapon(state, config, false, true, kTick);
    CHECK_FALSE(result.reload_started);

    // Fire once, then reload works and refills.
    game::update_weapon(state, config, true, false, kTick);
    CHECK(state.ammo == config.magazine_size - 1);
    result = game::update_weapon(state, config, false, true, kTick);
    CHECK(result.reload_started);
    for (int i = 0; i < 40 && state.reloading(); ++i) {
        game::update_weapon(state, config, false, false, kTick);
    }
    CHECK(state.ammo == config.magazine_size);
}

}  // namespace
