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
        } else if (key == "reload_seconds") {
            ok = parse_float(value, config.reload_seconds);
        } else if (key == "range") {
            ok = parse_float(value, config.range);
        } else if (key == "spread_degrees") {
            ok = parse_float(value, config.spread_degrees);
        } else {
            eng::log::warn("weapon config line {}: unknown key '{}' (skipped)", line_number, key);
        }
        if (!ok) {
            eng::log::error("weapon config line {}: invalid value '{}' for '{}'", line_number,
                            value, key);
            return std::nullopt;
        }
    }

    if (config.rounds_per_minute <= 0.0f || config.magazine_size <= 0 || config.damage <= 0.0f ||
        config.range <= 0.0f || config.reload_seconds < 0.0f) {
        eng::log::error("weapon config: values out of range");
        return std::nullopt;
    }
    return config;
}

WeaponTickResult update_weapon(WeaponState& state, const WeaponConfig& config, bool fire_held,
                               bool reload_requested, float dt) {
    WeaponTickResult result;

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

    if (reload_requested && state.ammo < config.magazine_size) {
        state.reload_remaining_seconds = config.reload_seconds;
        result.reload_started = true;
        return result;
    }

    if (fire_held && state.cooldown_seconds <= 0.0f) {
        if (state.ammo > 0) {
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

}  // namespace game
