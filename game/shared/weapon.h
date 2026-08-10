#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/shared/hitscan.h"

// Data-driven hitscan weapons: config parsed from key=value text assets, plus
// a deterministic tick-based state machine shared by client (prediction /
// offline) and the authoritative server.
namespace game {

inline constexpr std::size_t kMaxWeapons = 5;

struct WeaponConfig {
    std::string name = "rifle";
    float damage = 25.0f;
    float rounds_per_minute = 600.0f;
    int magazine_size = 30;
    float reload_seconds = 1.8f;
    float range = 100.0f;
    float spread_degrees = 0.0f;  // cone half-angle; 0 = perfectly accurate
    // Projectiles per trigger pull (shotguns > 1). Each pellet is an
    // independent hitscan ray and can damage a different target.
    int pellets = 1;
    // Damage multipliers by hit zone (game/shared/hitscan.h). Torso is always
    // 1.0: it is the baseline `damage` is quoted in, so a weapon that says
    // nothing about zones still does exactly what its config says on a body
    // shot. Per-weapon rather than global so a sniper can reward a head shot
    // more than an SMG does.
    float head_multiplier = 2.0f;
    float limb_multiplier = 0.75f;

    // Automatic weapons keep firing while held; semi-automatic ones require
    // a fresh trigger pull per shot.
    bool automatic = true;
    // Time to raise this weapon after switching to it.
    float switch_seconds = 0.4f;
    // Melee: no magazine, no reload, and `range` is arm's reach rather than
    // a ballistic limit. Modelled as a flag rather than as a gun with absurd
    // stats, because "magazine_size = 999999" would still dry-fire, still
    // auto-reload, and still put a round counter on the HUD.
    bool melee = false;
    // Fire sound, as a bare filename under assets/sounds/. Purely cosmetic:
    // the client reads it, the server never does. It lives here rather than
    // in a client-side slot->file table because the arsenal is already
    // data-driven, and a table keyed by slot would silently point at the
    // wrong gun the moment someone reorders the .cfg list. The default keeps
    // a weapon that says nothing about sound (the knife) audible.
    std::string fire_sound = "fire.wav";

    float shot_interval_seconds() const { return 60.0f / rounds_per_minute; }
};

// Damage scale for a hit in `zone`. Pure; the server applies it per pellet
// so a shotgun spray that catches a head and two legs is scored honestly.
inline float zone_damage_multiplier(const WeaponConfig& config, HitZone zone) {
    switch (zone) {
        case HitZone::Head:
            return config.head_multiplier;
        case HitZone::Arm:
        case HitZone::Leg:
            return config.limb_multiplier;
        case HitZone::Torso:
            break;
    }
    return 1.0f;
}

// Parses "key=value" lines ('#' comments, blank lines ok). Unknown keys are
// logged and skipped; malformed values fail the whole parse (nullopt).
std::optional<WeaponConfig> parse_weapon_config(std::string_view text);

struct WeaponState {
    int ammo = 0;
    float cooldown_seconds = 0.0f;          // until next shot allowed
    float reload_remaining_seconds = 0.0f;  // > 0 while reloading
    bool trigger_was_held = false;          // for semi-automatic gating

    bool reloading() const { return reload_remaining_seconds > 0.0f; }
};

struct WeaponTickResult {
    bool fired = false;
    bool dry_fired = false;
    bool reload_started = false;
    bool reload_finished = false;
    bool switched = false;
};

// Advances one weapon a tick. Deterministic; no I/O.
// Rules: no firing while reloading, on cooldown, or with an empty magazine;
// an empty-magazine trigger pull dry-fires once and auto-reloads; reload only
// starts when the magazine is not full; semi-automatic weapons need the
// trigger released between shots.
WeaponTickResult update_weapon(WeaponState& state, const WeaponConfig& config, bool fire_held,
                               bool reload_requested, float dt);

// A player's set of weapons plus which one is raised.
struct Loadout {
    std::array<WeaponState, kMaxWeapons> weapons{};
    std::uint8_t slot = 0;
    // > 0 while a weapon is being raised; blocks firing (but not reloading
    // progress on the newly held weapon).
    float switch_remaining_seconds = 0.0f;
};

// The weapons available in a match, in slot order.
struct Arsenal {
    std::vector<WeaponConfig> weapons;

    const WeaponConfig& at(std::uint8_t slot) const {
        return weapons[slot < weapons.size() ? slot : 0];
    }
    std::uint8_t clamp_slot(std::uint8_t slot) const { return slot < weapons.size() ? slot : 0; }
    bool empty() const { return weapons.empty(); }
    std::size_t size() const { return weapons.size(); }
};

// Fills every weapon in a loadout to a full magazine and raises slot 0.
void reset_loadout(Loadout& loadout, const Arsenal& arsenal);

// Advances the held weapon, handling switches. `desired_slot` is the client's
// requested slot (validated/clamped here); switching starts a raise timer and
// cancels any in-progress reload.
WeaponTickResult update_loadout(Loadout& loadout, const Arsenal& arsenal, std::uint8_t desired_slot,
                                bool fire_held, bool reload_requested, float dt);

}  // namespace game
