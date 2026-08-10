#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "engine/assets/paths.h"
#include "game/shared/rng.h"
#include "game/shared/weapon.h"

namespace {

using Catch::Approx;

constexpr float kTick = 1.0f / 60.0f;

game::Arsenal two_weapon_arsenal() {
    game::Arsenal arsenal;
    game::WeaponConfig rifle;
    rifle.name = "rifle";
    rifle.rounds_per_minute = 600.0f;  // 6 ticks between shots
    rifle.magazine_size = 10;
    rifle.switch_seconds = 0.2f;  // 12 ticks
    arsenal.weapons.push_back(rifle);

    game::WeaponConfig shotgun;
    shotgun.name = "shotgun";
    shotgun.rounds_per_minute = 60.0f;
    shotgun.magazine_size = 4;
    shotgun.pellets = 8;
    shotgun.spread_degrees = 5.0f;
    shotgun.automatic = false;
    shotgun.switch_seconds = 0.5f;
    arsenal.weapons.push_back(shotgun);
    return arsenal;
}

TEST_CASE("weapon config parses the new arsenal fields", "[weapon]") {
    const auto config = game::parse_weapon_config(R"(
name=boomstick
damage=11
rounds_per_minute=75
magazine_size=6
reload_seconds=2.6
range=40
spread_degrees=5.0
pellets=8
automatic=false
switch_seconds=0.5
)");
    REQUIRE(config.has_value());
    CHECK(config->pellets == 8);
    CHECK(config->spread_degrees == Approx(5.0f));
    CHECK_FALSE(config->automatic);
    CHECK(config->switch_seconds == Approx(0.5f));
}

TEST_CASE("weapon config carries a per-weapon fire sound", "[weapon]") {
    const auto named = game::parse_weapon_config("fire_sound=fire_shotgun.wav");
    REQUIRE(named.has_value());
    CHECK(named->fire_sound == "fire_shotgun.wav");

    // A weapon that says nothing about sound still has one: the knife relies
    // on this rather than shipping a gun report it has no business making.
    const auto silent = game::parse_weapon_config("damage=10");
    REQUIRE(silent.has_value());
    CHECK(silent->fire_sound == "fire.wav");
}

TEST_CASE("weapon config rejects a fire sound that leaves assets/sounds", "[weapon]") {
    // The value is joined onto assets/sounds/ at playback, so a separator or a
    // parent hop would let a config name any file on disk. Rejected at parse
    // time, where it fails the whole weapon loudly, rather than at playback,
    // where a missing file is swallowed as "sound is never fatal".
    CHECK_FALSE(game::parse_weapon_config("fire_sound=../../etc/passwd").has_value());
    CHECK_FALSE(game::parse_weapon_config("fire_sound=sub/dir.wav").has_value());
    CHECK_FALSE(game::parse_weapon_config("fire_sound=sub\\dir.wav").has_value());
    CHECK_FALSE(game::parse_weapon_config("fire_sound=").has_value());
}

TEST_CASE("every shipped weapon names a fire sound that exists", "[weapon]") {
    // Playback swallows a missing file (sound is never fatal), so a typo in a
    // .cfg or a renamed .wav would cost a weapon its voice with nothing in the
    // log to say so. This is the only place that mismatch gets caught.
    const auto root = eng::find_assets_root();
    REQUIRE(root.has_value());
    for (const char* weapon_name : {"rifle", "smg", "shotgun", "sniper", "knife"}) {
        const auto text =
            eng::read_text_file(*root / "weapons" / (std::string(weapon_name) + ".cfg"));
        REQUIRE(text.has_value());
        const auto config = game::parse_weapon_config(*text);
        REQUIRE(config.has_value());
        INFO(weapon_name << " -> " << config->fire_sound);
        CHECK(std::filesystem::is_regular_file(*root / "sounds" / config->fire_sound));
    }
}

TEST_CASE("weapon config rejects out-of-range arsenal fields", "[weapon]") {
    CHECK_FALSE(game::parse_weapon_config("pellets=0").has_value());
    CHECK_FALSE(game::parse_weapon_config("pellets=999").has_value());
    CHECK_FALSE(game::parse_weapon_config("spread_degrees=-1").has_value());
    CHECK_FALSE(game::parse_weapon_config("spread_degrees=90").has_value());
    CHECK_FALSE(game::parse_weapon_config("automatic=maybe").has_value());
}

TEST_CASE("semi-automatic weapons need a fresh trigger pull", "[weapon]") {
    game::WeaponConfig config;
    config.automatic = false;
    config.rounds_per_minute = 6000.0f;  // cooldown is not the limiter here
    config.magazine_size = 10;
    game::WeaponState state;
    state.ammo = 10;

    // Holding the trigger fires exactly once.
    int shots = 0;
    for (int i = 0; i < 30; ++i) {
        if (game::update_weapon(state, config, true, false, kTick).fired) {
            ++shots;
        }
    }
    CHECK(shots == 1);

    // Releasing and pulling again fires again.
    game::update_weapon(state, config, false, false, kTick);
    CHECK(game::update_weapon(state, config, true, false, kTick).fired);
}

TEST_CASE("automatic weapons keep firing while held", "[weapon]") {
    game::WeaponConfig config;
    config.automatic = true;
    config.rounds_per_minute = 600.0f;  // every 6 ticks
    config.magazine_size = 10;
    game::WeaponState state;
    state.ammo = 10;

    int shots = 0;
    for (int i = 0; i < 24; ++i) {
        if (game::update_weapon(state, config, true, false, kTick).fired) {
            ++shots;
        }
    }
    CHECK(shots == 4);
}

TEST_CASE("loadout switches weapons after the raise delay", "[weapon]") {
    const game::Arsenal arsenal = two_weapon_arsenal();
    game::Loadout loadout;
    game::reset_loadout(loadout, arsenal);
    CHECK(loadout.slot == 0);
    CHECK(loadout.weapons[0].ammo == 10);
    CHECK(loadout.weapons[1].ammo == 4);

    // Asking for slot 1 switches immediately but starts a raise timer.
    const auto result = game::update_loadout(loadout, arsenal, 1, false, false, kTick);
    CHECK(result.switched);
    CHECK(loadout.slot == 1);
    CHECK(loadout.switch_remaining_seconds > 0.0f);

    // Firing is blocked for the whole raise (0.5 s = 30 ticks).
    int shots_during_raise = 0;
    for (int i = 0; i < 28; ++i) {
        if (game::update_loadout(loadout, arsenal, 1, true, false, kTick).fired) {
            ++shots_during_raise;
        }
    }
    CHECK(shots_during_raise == 0);
    CHECK(loadout.weapons[1].ammo == 4);

    // The shotgun is semi-automatic, so even once raised it needs a fresh
    // pull: release, then squeeze.
    for (int i = 0; i < 5; ++i) {
        game::update_loadout(loadout, arsenal, 1, false, false, kTick);
    }
    int shots = 0;
    for (int i = 0; i < 5; ++i) {
        if (game::update_loadout(loadout, arsenal, 1, true, false, kTick).fired) {
            ++shots;
        }
    }
    CHECK(shots == 1);                     // exactly one shot per pull
    CHECK(loadout.weapons[1].ammo == 3);   // used the shotgun's mag
    CHECK(loadout.weapons[0].ammo == 10);  // rifle untouched
}

TEST_CASE("loadout ignores out-of-range slots", "[weapon]") {
    const game::Arsenal arsenal = two_weapon_arsenal();
    game::Loadout loadout;
    game::reset_loadout(loadout, arsenal);
    // Slot 3 does not exist in a 2-weapon arsenal: clamp to 0, never index
    // out of bounds.
    game::update_loadout(loadout, arsenal, 3, false, false, kTick);
    CHECK(loadout.slot == 0);
}

TEST_CASE("spread is deterministic and stays inside the cone", "[rng]") {
    const glm::vec3 forward{0.0f, 0.0f, -1.0f};
    const float cone = glm::radians(5.0f);

    // Same seed -> same direction, always (replay depends on this).
    const glm::vec3 a = game::spread_direction(forward, cone, 12345u);
    const glm::vec3 b = game::spread_direction(forward, cone, 12345u);
    CHECK(a.x == b.x);
    CHECK(a.y == b.y);
    CHECK(a.z == b.z);

    // Different seeds -> different directions, all within the cone.
    std::set<std::pair<float, float>> distinct;
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        const glm::vec3 dir = game::spread_direction(forward, cone, seed);
        CHECK(glm::length(dir) == Approx(1.0f).margin(1e-5f));
        const float angle = std::acos(std::clamp(glm::dot(dir, forward), -1.0f, 1.0f));
        CHECK(angle <= cone + 1e-4f);
        distinct.emplace(dir.x, dir.y);
    }
    CHECK(distinct.size() > 150);  // genuinely varied, not a constant
}

TEST_CASE("zero spread returns the exact aim direction", "[rng]") {
    const glm::vec3 forward = glm::normalize(glm::vec3{0.3f, -0.2f, -1.0f});
    const glm::vec3 dir = game::spread_direction(forward, 0.0f, 999u);
    CHECK(dir.x == forward.x);
    CHECK(dir.y == forward.y);
    CHECK(dir.z == forward.z);
}

}  // namespace
