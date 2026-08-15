#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/shared/hitscan.h"
#include "game/shared/input_command.h"

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

    // Damage falloff with distance. Full damage out to falloff_start_meters,
    // then linear down to falloff_min_fraction at falloff_end_meters and
    // beyond (still bounded by `range`, past which nothing is hit at all).
    //
    // The defaults are deliberately inert -- min_fraction 1.0 means no
    // falloff at any distance -- so a config that says nothing about falloff
    // behaves exactly as it did before this existed.
    float falloff_start_meters = 0.0f;
    float falloff_end_meters = 0.0f;
    float falloff_min_fraction = 1.0f;

    // --- aim down sights (M49) -------------------------------------------
    // Narrows the field of view and tightens the cone. Both default to 1.0,
    // meaning "this weapon has no sights", so a config written before ADS
    // existed behaves exactly as it did.
    //
    // ads_spread_scale is why this is not a client-side zoom effect: the
    // server owns the cone, so it has to know the aim state and run the same
    // transition the client predicts.
    float ads_fov_scale = 1.0f;     // 0.6 = a 40% narrower FOV while aimed
    float ads_spread_scale = 1.0f;  // multiplies spread_degrees while aimed
    float ads_seconds = 0.22f;      // time to raise and to lower the sights

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

// Fraction of full damage a hit at `distance` metres keeps: 1.0 inside the
// falloff start, falling linearly to falloff_min_fraction at the end and
// staying there.
//
// Every comparison is written so that a NaN distance or a nonsense config
// falls through to 1.0 rather than propagating into a damage number.
inline float damage_falloff_scale(const WeaponConfig& config, float distance) {
    const float floor_fraction = std::clamp(config.falloff_min_fraction, 0.0f, 1.0f);
    if (!(config.falloff_end_meters > config.falloff_start_meters) || floor_fraction >= 1.0f) {
        return 1.0f;  // falloff disabled, which is the default
    }
    if (!(distance > config.falloff_start_meters)) {
        return 1.0f;  // inside the full-damage band (and the NaN case)
    }
    if (distance >= config.falloff_end_meters) {
        return floor_fraction;
    }
    const float t = (distance - config.falloff_start_meters) /
                    (config.falloff_end_meters - config.falloff_start_meters);
    return 1.0f - t * (1.0f - floor_fraction);
}

// Base damage for one hit at `distance`, before any hit-zone multiplier.
inline float damage_at_distance(const WeaponConfig& config, float distance) {
    return config.damage * damage_falloff_scale(config, distance);
}

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
    // How far into the sights the player is, 0 (hip) to 1 (fully aimed).
    // Lives on the loadout rather than the weapon because it is a property of
    // the player's posture, and it must collapse to 0 the moment a different
    // weapon comes up -- otherwise switching from a scoped sniper to a knife
    // would leave the world zoomed.
    float ads_fraction = 0.0f;
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
// `aim_held` defaults to false so that every caller written before ADS
// existed -- including the tests -- keeps its old meaning exactly.
WeaponTickResult update_loadout(Loadout& loadout, const Arsenal& arsenal, std::uint8_t desired_slot,
                                bool fire_held, bool reload_requested, float dt,
                                bool aim_held = false);

// True when this weapon has sights worth raising. A weapon that neither zooms
// nor tightens its cone would otherwise play the whole raise/lower animation
// to accomplish nothing -- and the knife would slow you down for free.
constexpr bool supports_ads(const WeaponConfig& config) {
    return !config.melee && (config.ads_fov_scale < 1.0f || config.ads_spread_scale < 1.0f);
}

// The rule for whether the player is aiming this tick, in shared code so the
// client's prediction and the server's authority cannot drift apart.
//
// Sprinting wins. That is the whole cost of aiming: not a speed penalty
// bolted onto the movement code, but the fact that you cannot be sprinting
// and aimed at the same time.
constexpr bool wants_ads(const InputCommand& command, bool sprinting) {
    return has_button(command, Button::Aim) && !sprinting;
}

// Interpolates between the hip and aimed values of a scale, both of which are
// 1.0-relative. Used for FOV on the client and for the cone on the server,
// from the SAME ads_fraction, so what the crosshair promises is what the
// server rolls.
constexpr float ads_lerp(float scale_at_full_aim, float fraction) {
    return 1.0f + (scale_at_full_aim - 1.0f) * fraction;
}

// The cone half-angle to actually roll, in degrees.
constexpr float effective_spread_degrees(const WeaponConfig& config, float ads_fraction) {
    return config.spread_degrees * ads_lerp(config.ads_spread_scale, ads_fraction);
}

}  // namespace game
