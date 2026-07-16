#pragma once

#include <optional>
#include <string>
#include <string_view>

// Data-driven hitscan weapon: config parsed from a key=value text asset,
// plus a deterministic tick-based state machine shared by client
// (prediction/offline) and the authoritative server.
namespace game {

struct WeaponConfig {
    std::string name = "rifle";
    float damage = 25.0f;
    float rounds_per_minute = 600.0f;
    int magazine_size = 30;
    float reload_seconds = 1.8f;
    float range = 100.0f;
    float spread_degrees = 0.0f;  // reserved; 0 = perfectly accurate

    float shot_interval_seconds() const { return 60.0f / rounds_per_minute; }
};

// Parses "key=value" lines ('#' comments, blank lines ok). Unknown keys are
// logged and skipped; malformed values fail the whole parse (nullopt).
std::optional<WeaponConfig> parse_weapon_config(std::string_view text);

struct WeaponState {
    int ammo = 0;
    float cooldown_seconds = 0.0f;         // until next shot allowed
    float reload_remaining_seconds = 0.0f; // > 0 while reloading

    bool reloading() const { return reload_remaining_seconds > 0.0f; }
};

struct WeaponTickResult {
    bool fired = false;
    bool dry_fired = false;
    bool reload_started = false;
    bool reload_finished = false;
};

// Advances the weapon one tick. Deterministic; no I/O.
// Rules: no firing while reloading or on cooldown or with an empty mag;
// an empty-mag trigger pull dry-fires once per cooldown and auto-reloads;
// reload is only started when the magazine is not full.
WeaponTickResult update_weapon(WeaponState& state, const WeaponConfig& config, bool fire_held,
                               bool reload_requested, float dt);

}  // namespace game
