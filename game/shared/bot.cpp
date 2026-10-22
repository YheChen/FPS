#include "game/shared/bot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "game/shared/grenade.h"
#include "game/shared/rng.h"

namespace game {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Bots use the gameplay RNG deliberately, unlike particles: their decisions
// feed advance_player, so they are part of the simulation and must be
// reproducible for replays to work.
float unit_random(std::uint32_t seed, std::uint32_t salt) {
    return hash_float01(hash_combine(seed, salt));
}

float horizontal_distance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2{a.x - b.x, a.z - b.z});
}

// Presses the movement buttons that carry the bot toward `direction` (a
// unit vector in the ground plane) while it keeps looking along `yaw`. The
// route is in world space and the buttons are in the view's frame, so the
// direction is split into forward and sideways parts and each is pressed
// past a threshold: cos(67.5 degrees), which quantizes the plane into the
// eight headings the buttons can express. A bot walking a route at 20
// degrees off its gaze alternates between two of them tick to tick, which
// is what a player does.
void press_toward(InputCommand& command, const glm::vec2& direction, float yaw) {
    const glm::vec2 forward{std::sin(yaw), -std::cos(yaw)};  // (x, z), as wish_direction()
    const glm::vec2 right{std::cos(yaw), std::sin(yaw)};
    const float ahead = glm::dot(direction, forward);
    const float sideways = glm::dot(direction, right);
    constexpr float kSector = 0.3827f;
    set_button(command, Button::Forward, ahead > kSector);
    set_button(command, Button::Back, ahead < -kSector);
    set_button(command, Button::Right, sideways > kSector);
    set_button(command, Button::Left, sideways < -kSector);
}

}  // namespace

std::optional<std::size_t> nearest_enemy(const glm::vec3& from, Team team,
                                         std::span<const BotTarget> candidates) {
    std::optional<std::size_t> best;
    float nearest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const BotTarget& candidate = candidates[i];
        // Dead players and teammates are both invisible to targeting. A
        // teammate is not "a target worth less" -- it is not a target, and a
        // bot that ranks one at all will pick it whenever it is closer.
        if (!candidate.alive || candidate.team == team) {
            continue;
        }
        const float distance = glm::length(candidate.position - from);
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

BotConfig bot_config_for(BotSkill skill) {
    BotConfig config;  // the Normal defaults
    switch (skill) {
        case BotSkill::Easy:
            config.turn_speed = 2.2f;
            config.reaction_seconds = 0.70f;
            config.aim_error_radians = 0.140f;
            config.aim_drift_hz = 0.40f;
            config.burst_seconds = 0.18f;
            config.burst_pause_seconds = 1.40f;
            break;
        case BotSkill::Normal:
            break;
        case BotSkill::Hard:
            config.turn_speed = 5.5f;
            config.reaction_seconds = 0.16f;
            config.aim_error_radians = 0.030f;
            config.aim_drift_hz = 0.85f;
            config.burst_seconds = 0.40f;
            config.burst_pause_seconds = 0.40f;
            break;
        case BotSkill::Deadly:
            // The old behaviour exactly: instant reaction, perfect aim, and a
            // trigger that is never released.
            config.turn_speed = 8.0f;
            config.reaction_seconds = 0.0f;
            config.aim_error_radians = 0.0f;
            config.fire_cone_radians = 0.10f;
            config.burst_seconds = 1e9f;
            config.burst_pause_seconds = 0.0f;
            break;
    }
    return config;
}

const char* bot_skill_name(BotSkill skill) {
    switch (skill) {
        case BotSkill::Easy:
            return "easy";
        case BotSkill::Normal:
            return "normal";
        case BotSkill::Hard:
            return "hard";
        case BotSkill::Deadly:
            return "deadly";
    }
    return "normal";
}

std::optional<BotSkill> bot_skill_from_name(std::string_view name) {
    if (name == "easy") {
        return BotSkill::Easy;
    }
    if (name == "normal") {
        return BotSkill::Normal;
    }
    if (name == "hard") {
        return BotSkill::Hard;
    }
    if (name == "deadly") {
        return BotSkill::Deadly;
    }
    return std::nullopt;
}

float yaw_towards(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 delta = to - from;
    // Matches view_direction(): x = sin(yaw), z = -cos(yaw).
    return std::atan2(delta.x, -delta.z);
}

float shortest_angle_delta(float current, float target) {
    float delta = std::fmod(target - current + kPi, 2.0f * kPi);
    if (delta < 0.0f) {
        delta += 2.0f * kPi;
    }
    return delta - kPi;
}

InputCommand decide(BotState& state, const BotSenses& senses, const BotConfig& config, float dt,
                    std::uint32_t seed) {
    if (!state.initialized) {
        state.aim_yaw = senses.yaw;
        state.wander_yaw = senses.yaw;
        // Fixed for this bot's lifetime, so bots sharing a config sway out of
        // phase with each other.
        state.aim_phase = unit_random(seed, 0xc2b2ae35u) * 2.0f * kPi;
        state.initialized = true;
    }
    state.aim_time += dt;

    InputCommand command;
    command.weapon_slot = config.weapon_slot;

    const float distance =
        senses.has_target ? glm::length(senses.target_position - senses.position) : 0.0f;
    const bool engaging = senses.has_target && distance <= config.sight_range;

    // A route with nothing to shoot at is walked facing where it goes. The
    // timer is topped up so the idle heading change below never fires over
    // it: a bot does not glance at a random wall mid-route.
    if (!engaging && senses.has_waypoint) {
        state.wander_yaw = yaw_towards(senses.position, senses.waypoint);
        state.wander_timer = config.wander_seconds;
    }

    // --- reaction ----------------------------------------------------------
    // A target has to be seen for a while before the bot does anything about
    // it. Held rather than reset while a known target is merely occluded, so
    // ducking behind a pillar for a moment does not buy a fresh reaction
    // delay every time.
    if (!senses.has_target) {
        state.target_seen_seconds = 0.0f;
    } else if (senses.target_visible) {
        state.target_seen_seconds += dt;
    }
    const bool reacted = state.target_seen_seconds >= config.reaction_seconds;

    // --- aim ---------------------------------------------------------------
    // `true_*` is where the target actually is; the bot aims at that plus an
    // error, and its fire gate is judged against the true value. That split is
    // the whole mechanism: the fire cone finally means something, because the
    // aim no longer sits exactly on the target by construction.
    float true_yaw = state.wander_yaw;
    float true_pitch = 0.0f;
    if (engaging) {
        true_yaw = yaw_towards(senses.position, senses.target_position);
        // Aim at chest height rather than at the feet the position refers to.
        const glm::vec3 aim_point = senses.target_position + glm::vec3{0.0f, 1.1f, 0.0f};
        const glm::vec3 eye = senses.position + glm::vec3{0.0f, 1.6f, 0.0f};
        const float horizontal = glm::length(glm::vec2{aim_point.x - eye.x, aim_point.z - eye.z});
        true_pitch = horizontal > 0.01f ? std::atan2(aim_point.y - eye.y, horizontal) : 0.0f;
    }

    // Two incommensurate frequencies, so the sway neither looks like a sine
    // wave nor repeats on a period a player could learn. Weights sum to 1, so
    // the error stays inside +-aim_error_radians.
    float desired_yaw = true_yaw;
    float desired_pitch = true_pitch;
    if (engaging && reacted && config.aim_error_radians > 0.0f) {
        const float w = 2.0f * kPi * config.aim_drift_hz * state.aim_time;
        const float p = state.aim_phase;
        const float sway_yaw = 0.62f * std::sin(w + p) + 0.38f * std::sin(1.73f * w + 2.1f * p);
        const float sway_pitch =
            0.62f * std::sin(0.87f * w + 1.4f * p) + 0.38f * std::sin(1.31f * w + 0.6f * p);
        desired_yaw = true_yaw + config.aim_error_radians * sway_yaw;
        // Vertically a player is a much smaller target than horizontally, so
        // the same angular error here would make bots miss high or low almost
        // always. Halved to keep the sway a challenge rather than a wall.
        desired_pitch = true_pitch + 0.5f * config.aim_error_radians * sway_pitch;
    }

    // Before reacting the bot keeps whatever it was already doing rather than
    // freezing: a target appearing must not stop it mid-stride.
    if (engaging && !reacted) {
        desired_yaw = state.aim_yaw;
        desired_pitch = state.aim_pitch;
    }

    // Turn toward the aim point at a bounded rate. Snapping instantly would
    // make bots inhumanly accurate and would also skip the whole point of
    // having a fire cone.
    const float yaw_delta = shortest_angle_delta(state.aim_yaw, desired_yaw);
    const float max_turn = config.turn_speed * dt;
    state.aim_yaw += std::clamp(yaw_delta, -max_turn, max_turn);
    state.aim_pitch += std::clamp(desired_pitch - state.aim_pitch, -max_turn, max_turn);
    state.aim_pitch = std::clamp(state.aim_pitch, -1.5f, 1.5f);

    command.yaw = state.aim_yaw;
    command.pitch = state.aim_pitch;

    // --- movement ----------------------------------------------------------
    state.strafe_timer -= dt;
    if (state.strafe_timer <= 0.0f) {
        state.strafe_timer = config.strafe_seconds;
        state.strafe_sign = unit_random(seed, 0x51ed270bu) < 0.5f ? -1.0f : 1.0f;
    }

    // Holding a working range on a visible target is the fight itself, and
    // the route has no say in it. The route is for everything else: getting
    // to an enemy it cannot see, closing on one it can but is too far from,
    // and having somewhere to be when there is no enemy at all.
    const bool fighting =
        senses.has_target && senses.target_visible && distance <= config.engage_range;
    const glm::vec2 to_waypoint{senses.waypoint.x - senses.position.x,
                                senses.waypoint.z - senses.position.z};
    const bool following = senses.has_waypoint && glm::dot(to_waypoint, to_waypoint) > 1e-6f;
    if (fighting) {
        // Circle-strafe, backing off to hold a working range.
        set_button(command, state.strafe_sign > 0.0f ? Button::Right : Button::Left, true);
        if (distance < config.preferred_range) {
            set_button(command, Button::Back, true);
        }
        if (senses.on_ground && unit_random(seed, 0x9e3779b9u) < config.jump_chance) {
            set_button(command, Button::Jump, true);
        }
    } else if (following) {
        const glm::vec2 direction = glm::normalize(to_waypoint);
        if (senses.has_target) {
            // Looking at (or for) the enemy while the feet follow the route.
            press_toward(command, direction, state.aim_yaw);
            // Sprint only works pushing forward, and a route leg that runs
            // mostly away from the gaze is walked rather than fumbled.
            const glm::vec2 forward{std::sin(state.aim_yaw), -std::cos(state.aim_yaw)};
            set_button(command, Button::Sprint, glm::dot(direction, forward) > 0.7f);
            if (senses.on_ground && unit_random(seed, 0x9e3779b9u) < config.jump_chance) {
                set_button(command, Button::Jump, true);
            }
        } else {
            // Nothing to face: the gaze was already turned down the route.
            set_button(command, Button::Forward, true);
        }
    } else if (senses.has_target) {
        // No route -- off the grid, or none exists -- so the pursuit this
        // shipped with: strafe, close, hold.
        set_button(command, state.strafe_sign > 0.0f ? Button::Right : Button::Left, true);
        if (distance > config.engage_range) {
            set_button(command, Button::Forward, true);
            set_button(command, Button::Sprint, true);
        } else if (distance < config.preferred_range) {
            set_button(command, Button::Back, true);
        }
        if (senses.on_ground && unit_random(seed, 0x9e3779b9u) < config.jump_chance) {
            set_button(command, Button::Jump, true);
        }
    } else {
        // Idle with no route: pick a heading every so often and walk it.
        state.wander_timer -= dt;
        if (state.wander_timer <= 0.0f) {
            state.wander_timer = config.wander_seconds;
            state.wander_yaw = (unit_random(seed, 0x85ebca6bu) * 2.0f - 1.0f) * kPi;
        }
        set_button(command, Button::Forward, true);
    }

    // Walls win over whatever the plan was: a bot grinding into a corner for
    // the rest of the match is the most obvious way this can look broken.
    // Only when steering blind, though. A route already knows where the walls
    // are, and backing away from one it is walking beside -- which the ray
    // sees whenever the gaze crosses it -- would undo the route a tick at a
    // time. A route that really is wrong is caught by navigate()'s stuck rule.
    if (!following && senses.forward_clearance < config.wall_avoid_distance) {
        set_button(command, Button::Forward, false);
        set_button(command, Button::Back, true);
        state.wander_yaw = state.aim_yaw + (state.strafe_sign > 0.0f ? 1.2f : -1.2f);
        state.wander_timer = config.wander_seconds;
    }

    // --- shooting ----------------------------------------------------------
    // Gated on the aim having caught up with where the bot is TRYING to point
    // -- `desired_*`, which includes its error -- not on where the target
    // actually is.
    //
    // That distinction is the whole difference between an aim-error knob that
    // works and one that does nothing. Gating on the true bearing means the
    // bot only pulls the trigger during the instants its sway happens to be
    // near zero, so every shot it takes is still a good one and more error
    // just means fewer, equally deadly bursts. Gating on its own intent makes
    // it shoot confidently while pointing slightly wrong, which is what
    // missing actually is.
    //
    // Pitch counts too. It never used to, and with a vertical error now in
    // play a bot would otherwise fire over a target's head whenever its yaw
    // was good.
    const bool on_target =
        std::abs(shortest_angle_delta(state.aim_yaw, desired_yaw)) < config.fire_cone_radians &&
        std::abs(state.aim_pitch - desired_pitch) < config.fire_cone_radians;
    const bool wants_to_shoot = engaging && senses.target_visible && reacted && on_target;

    // Trigger discipline. The cycle only advances while the bot wants to
    // shoot, so a pause is not burned through while it is hunting -- and it
    // resets between engagements, so re-acquiring a target does not start
    // mid-pause.
    if (!wants_to_shoot) {
        state.bursting = false;
        state.burst_timer = 0.0f;
    } else {
        state.burst_timer -= dt;
        if (state.burst_timer <= 0.0f) {
            state.bursting = !state.bursting;
            state.burst_timer = state.bursting ? config.burst_seconds : config.burst_pause_seconds;
        }
    }
    if (wants_to_shoot && state.bursting) {
        set_button(command, Button::Fire, true);
    }

    // --- grenades -----------------------------------------------------------
    //
    // Committed to ONCE and then held, rather than re-decided every tick. The
    // cook duration IS the decision: rolling it again each tick would average
    // every bot to the same fuse and erase the variety this exists for.
    if (state.grenade_hold_seconds >= 0.0f) {
        state.grenade_hold_seconds -= dt;
        if (state.grenade_hold_seconds >= 0.0f) {
            set_button(command, Button::Grenade, true);
            // A grenade is thrown along the LOOK direction, and a bot aiming
            // at a chest puts it on the floor in front of itself. While the
            // pin is out it aims along the arc instead -- which also means it
            // is visibly not shooting at you, and that is a fair trade to
            // give away.
            if (const auto pitch = grenade_launch_pitch(distance)) {
                command.pitch = *pitch;
            }
            set_button(command, Button::Fire, false);
        }
        // Falling below zero releases the button, and the release is the
        // throw: the server reads the two edges, so nothing here has to say
        // "throw" at all.
        return command;
    }

    const bool can_throw = engaging && senses.target_visible && reacted && senses.has_grenade &&
                           distance >= config.grenade_min_range &&
                           grenade_launch_pitch(distance).has_value();
    if (can_throw && unit_random(seed, 0x2545f491u) < config.grenade_chance_per_second * dt) {
        // Half the throws are not cooked at all -- the pin comes out and it
        // leaves immediately. The rest are held for a roll up to the cap,
        // which stays well short of the fuse: a bot that kills itself is funny
        // once and then just hands over a free kill.
        const bool cook = unit_random(seed, 0x9e3779b1u) < config.grenade_cook_chance;
        const float held =
            cook ? unit_random(seed, 0x27d4eb2fu) * config.grenade_cook_max_seconds : 0.0f;
        // At least one tick either way, or the button never goes down and the
        // server never sees a pin pulled.
        state.grenade_hold_seconds = std::max(held, dt);
        set_button(command, Button::Grenade, true);
        set_button(command, Button::Fire, false);
    }

    return command;
}

std::optional<glm::vec3> navigate(BotState& state, const NavGrid& grid, const glm::vec3& position,
                                  const std::optional<glm::vec3>& chase, const BotConfig& config,
                                  float dt, std::uint32_t seed) {
    BotRoute& route = state.route;
    if (grid.empty()) {
        route = BotRoute{};
        return std::nullopt;
    }
    route.repath_timer -= dt;
    const auto done = [&] { return route.next >= route.points.size(); };
    // Planning leaves the stuck clock alone: a chase replans every half
    // second, and a clock that restarted with each plan would never reach
    // the limit. Only progress resets it.
    const auto plan = [&](const glm::vec3& goal, bool roaming) {
        route.points = find_path(grid, position, goal);
        route.next = 0;
        route.goal = goal;
        route.roaming = roaming;
        route.repath_timer = config.route_repath_seconds;
    };

    if (chase) {
        // On the timer, when the route was a roam, or when the target has
        // moved well away from where this route was going. Not on every tick
        // the target moves: a sprinting target moves a cell a tick, and a
        // route to where it was a moment ago still rounds the same corner.
        const bool stale = route.roaming || route.repath_timer <= 0.0f ||
                           horizontal_distance(*chase, route.goal) > 3.0f;
        if (stale) {
            plan(*chase, /*roaming=*/false);
        }
    } else if (!route.roaming || done()) {
        // Nowhere to be. Somewhere reachable, and a real walk away; a few
        // rolls, and the last one stands if none is far enough -- a small
        // map's longest walk is still a walk.
        glm::vec3 goal = position;
        for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
            const auto candidate =
                random_walkable_point(grid, hash_combine(seed, 0x6a09e667u + attempt), position);
            if (!candidate) {
                break;
            }
            goal = *candidate;
            if (horizontal_distance(goal, position) >= config.route_roam_min_distance) {
                break;
            }
        }
        plan(goal, /*roaming=*/true);
    }

    // Waypoints reached fall away. Several can go at once: a bot pushed
    // sideways by a fight may find itself past the next two corners.
    while (!done() && horizontal_distance(position, route.points[route.next]) <=
                          config.route_waypoint_radius) {
        ++route.next;
    }
    if (done()) {
        return std::nullopt;
    }

    // Stuck: somewhere to go and no progress toward it. The route is dropped
    // outright. A chase replans at once next tick, a roam picks somewhere
    // else, and either beats leaning on the same lip for the rest of the
    // match.
    if (horizontal_distance(position, route.progress_anchor) >= config.route_stuck_distance) {
        route.progress_anchor = position;
        route.stuck_timer = 0.0f;
    } else {
        route.stuck_timer += dt;
        if (route.stuck_timer >= config.route_stuck_seconds) {
            route = BotRoute{};
            route.progress_anchor = position;
            return std::nullopt;
        }
    }
    return route.points[route.next];
}

}  // namespace game
