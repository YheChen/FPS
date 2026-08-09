#include "game/shared/weapon.h"

#include <algorithm>
#include <charconv>

#include "engine/core/log.h"

namespace game {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool parse_float(std::string_view value, float& out) {
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && ptr == value.data() + value.size();
}

bool parse_int(std::string_view value, int& out) {
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && ptr == value.data() + value.size();
}

}  // namespace

std::optional<WeaponConfig> parse_weapon_config(std::string_view text) {
    WeaponConfig config;
    std::string_view rest = text;
    int line_number = 0;
    while (!rest.empty()) {
        ++line_number;
        const std::size_t newline = rest.find('\n');
        std::string_view line = trim(rest.substr(0, newline));
        rest = (newline == std::string_view::npos) ? std::string_view{} : rest.substr(newline + 1);

        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            eng::log::error("weapon config line {}: expected key=value, got '{}'", line_number,
                            line);
            return std::nullopt;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));

        bool ok = true;
        if (key == "name") {
            config.name = std::string(value);
        } else if (key == "damage") {
            ok = parse_float(value, config.damage);
        } else if (key == "rounds_per_minute") {
            ok = parse_float(value, config.rounds_per_minute);
        } else if (key == "magazine_size") {
            ok = parse_int(value, config.magazine_size);
        } else if (key == "melee") {
            if (value == "true" || value == "1") {
                config.melee = true;
            } else if (value == "false" || value == "0") {
                config.melee = false;
            } else {
                ok = false;
            }
        } else if (key == "reload_seconds") {
            ok = parse_float(value, config.reload_seconds);
        } else if (key == "range") {
            ok = parse_float(value, config.range);
        } else if (key == "spread_degrees") {
            ok = parse_float(value, config.spread_degrees);
        } else if (key == "pellets") {
            ok = parse_int(value, config.pellets);
        } else if (key == "switch_seconds") {
            ok = parse_float(value, config.switch_seconds);
        } else if (key == "automatic") {
            if (value == "true" || value == "1") {
                config.automatic = true;
            } else if (value == "false" || value == "0") {
                config.automatic = false;
            } else {
                ok = false;
            }
        } else {
            eng::log::warn("weapon config line {}: unknown key '{}' (skipped)", line_number, key);
        }
        if (!ok) {
            eng::log::error("weapon config line {}: invalid value '{}' for '{}'", line_number,
                            value, key);
            return std::nullopt;
        }
    }

    // A melee weapon legitimately has no magazine, so the usual "> 0" rule
    // would reject the one config that means it.
    if (config.magazine_size < 0 || (!config.melee && config.magazine_size <= 0)) {
        eng::log::error("weapon config: magazine_size must be > 0 unless melee=true");
        return std::nullopt;
    }
    if (config.rounds_per_minute <= 0.0f || config.damage <= 0.0f || config.range <= 0.0f ||
        config.reload_seconds < 0.0f || config.pellets <= 0 || config.pellets > 32 ||
        config.spread_degrees < 0.0f || config.spread_degrees > 45.0f ||
        config.switch_seconds < 0.0f) {
        eng::log::error("weapon config: values out of range");
        return std::nullopt;
    }
    return config;
}

WeaponTickResult update_weapon(WeaponState& state, const WeaponConfig& config, bool fire_held,
                               bool reload_requested, float dt) {
    WeaponTickResult result;
    // Semi-automatic weapons need a fresh pull: the trigger must have been
    // released since the last shot.
    const bool trigger_pulled = fire_held && (config.automatic || !state.trigger_was_held);
    state.trigger_was_held = fire_held;

    state.cooldown_seconds = std::max(0.0f, state.cooldown_seconds - dt);

    if (state.reloading()) {
        state.reload_remaining_seconds -= dt;
        if (state.reload_remaining_seconds <= 0.0f) {
            state.reload_remaining_seconds = 0.0f;
            state.ammo = config.magazine_size;
            result.reload_finished = true;
        }
        return result;  // nothing else while reloading
    }

    // A knife has nothing to reload and never runs dry, so both the explicit
    // reload and the empty-magazine path below are skipped for it.
    if (!config.melee && reload_requested && state.ammo < config.magazine_size) {
        state.reload_remaining_seconds = config.reload_seconds;
        result.reload_started = true;
        return result;
    }

    if (trigger_pulled && state.cooldown_seconds <= 0.0f) {
        if (config.melee) {
            state.cooldown_seconds = config.shot_interval_seconds();
            result.fired = true;
        } else if (state.ammo > 0) {
            --state.ammo;
            state.cooldown_seconds = config.shot_interval_seconds();
            result.fired = true;
        } else {
            state.cooldown_seconds = config.shot_interval_seconds();
            result.dry_fired = true;
            // Auto-reload on an empty trigger pull.
            state.reload_remaining_seconds = config.reload_seconds;
            result.reload_started = true;
        }
    }
    return result;
}

void reset_loadout(Loadout& loadout, const Arsenal& arsenal) {
    loadout = {};
    for (std::size_t i = 0; i < kMaxWeapons; ++i) {
        loadout.weapons[i].ammo = i < arsenal.size() ? arsenal.weapons[i].magazine_size : 0;
    }
}

WeaponTickResult update_loadout(Loadout& loadout, const Arsenal& arsenal, std::uint8_t desired_slot,
                                bool fire_held, bool reload_requested, float dt) {
    WeaponTickResult result;
    if (arsenal.empty()) {
        return result;
    }

    // A switch request is just "the newest slot the client asked for", so a
    // dropped input packet cannot lose a weapon change.
    const std::uint8_t wanted = arsenal.clamp_slot(desired_slot);
    if (wanted != loadout.slot) {
        loadout.slot = wanted;
        loadout.switch_remaining_seconds = arsenal.at(wanted).switch_seconds;
        // Switching interrupts a reload on the weapon being stowed; the new
        // weapon comes up in whatever state it was left in.
        loadout.weapons[loadout.slot].trigger_was_held = true;  // require a fresh pull
        result.switched = true;
    }

    WeaponState& state = loadout.weapons[loadout.slot];
    const WeaponConfig& config = arsenal.at(loadout.slot);

    if (loadout.switch_remaining_seconds > 0.0f) {
        loadout.switch_remaining_seconds = std::max(0.0f, loadout.switch_remaining_seconds - dt);
        // Weapon is still coming up: no firing, no reloading, but the cooldown
        // and trigger state keep tracking so the transition is seamless.
        state.cooldown_seconds = std::max(0.0f, state.cooldown_seconds - dt);
        state.trigger_was_held = fire_held;
        return result;
    }

    const WeaponTickResult shot = update_weapon(state, config, fire_held, reload_requested, dt);
    result.fired = shot.fired;
    result.dry_fired = shot.dry_fired;
    result.reload_started = shot.reload_started;
    result.reload_finished = shot.reload_finished;
    return result;
}

}  // namespace game
