#pragma once

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "game/shared/input_command.h"
#include "game/shared/protocol.h"

// Grenades (M55). ONE per life, thrown with G, and COOKED by holding G.
//
// The cook is the whole design. Pulling the pin starts a fuse that does not
// care whether the grenade is still in your hand, so holding it trades the
// enemy's time to run for your own margin of error. Throw it fresh and they
// walk away from it; cook it and they cannot; cook it too long and it kills
// you. That is a decision worth making every time, out of one button.
//
// Everything here is pure and shared for the usual reason: the client predicts
// the arc it draws and the server rolls the damage, and the two must not
// disagree about where a grenade went or what it cost.
namespace game {

// One grenade per life, restored on respawn -- never over time and never by
// pickup. Scarcity is what makes cooking a decision rather than a habit.
inline constexpr std::uint8_t kGrenadesPerLife = 1;

// Seconds from pulling the pin to detonation, in hand or not.
inline constexpr float kGrenadeFuseSeconds = 3.0f;
// How hard it leaves the hand, and from where relative to the eye.
//
// 24 m/s, not the 16 M55 shipped with. A throw only reaches v^2/g -- 11.6 m at
// 16 m/s -- and this arena is not fought at 11 metres: instrumenting bots put
// their engagements at 23-31 m, and the bot config aims for a band between
// preferred_range 11 and engage_range 26. A grenade that could not cross that
// gap was a grenade nobody could use without first walking into knife range,
// which is not a trade anyone would make.
//
// 24 m/s reaches 26 m, which is engage_range exactly. That is the number this
// should have been keyed to from the start.
inline constexpr float kGrenadeThrowSpeed = 24.0f;
inline constexpr float kGrenadeGravity = 22.0f;  // matches the player's fall
inline constexpr float kGrenadeRadius = 0.12f;
// A throw starts slightly ahead of the eye so it does not spawn inside the
// thrower's own capsule and immediately "bounce".
inline constexpr float kGrenadeThrowOffset = 0.45f;
// Bounce: how much speed survives a wall, and how much of the tangent does.
inline constexpr float kGrenadeRestitution = 0.42f;
inline constexpr float kGrenadeFriction = 0.72f;

// Damage. Full at the centre, falling to zero at the edge -- nobody is killed
// by a grenade that went off across the room, and standing on one is fatal.
inline constexpr float kGrenadeDamage = 110.0f;  // > 100: a direct hit kills
inline constexpr float kGrenadeBlastRadius = 5.5f;

// Damage at a distance from the blast. Linear falloff, not inverse-square:
// inverse-square spends almost all of its range doing nothing, which reads as
// a grenade that does not work rather than one that missed.
constexpr float grenade_damage_at(float distance) {
    if (distance >= kGrenadeBlastRadius) {
        return 0.0f;
    }
    const float t = distance <= 0.0f ? 0.0f : distance / kGrenadeBlastRadius;
    return kGrenadeDamage * (1.0f - t);
}

// Who a blast is allowed to hurt. It hurts YOU -- a grenade cooked too long,
// or thrown at a wall a metre away, has to be able to kill the person who
// threw it, or cooking costs nothing and the decision disappears. It does not
// hurt teammates, for the same reason bullets do not (M52).
constexpr bool blast_can_damage(std::uint8_t thrower, Team thrower_team, std::uint8_t victim,
                                Team victim_team) {
    return victim == thrower || victim_team != thrower_team;
}

// True while the pin is out. The server derives this from the button rather
// than trusting a reported cook time.
constexpr bool wants_grenade(const InputCommand& command) {
    return has_button(command, Button::Grenade);
}

// A grenade in flight, as both sides model it.
struct GrenadeState {
    std::uint8_t id = 0;
    std::uint8_t thrower = kNoPlayer;
    Team team = Team::A;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float fuse_remaining = kGrenadeFuseSeconds;
    bool in_hand = true;  // pin pulled, not yet released
};

// The velocity a throw leaves with, from where the thrower is looking.
glm::vec3 grenade_throw_velocity(float yaw, float pitch, const glm::vec3& thrower_velocity);

// The pitch that lands a throw `horizontal_distance` away, or nullopt when it
// is out of reach however you aim.
//
// A grenade is not a bullet and a thrower who aims AT something misses it: at
// kGrenadeThrowSpeed a level throw from eye height is on the floor after about
// nine metres. Anything that wants to land one near a target -- a bot, or a
// future aim assist -- has to solve for the arc rather than point at the
// thing, and that is a rule rather than a bot's private habit.
//
// The flat-ground solution to sin(2*theta) = g*d / v^2, taking the LOW arc:
// the high one lobs over cover and hangs long enough for anyone to walk away,
// which is the wrong grenade almost every time.
//
// Maximum reach is v^2/g, about 26 m with the current numbers, and past that
// there is no angle at all -- which is why this returns an optional rather
// than a best effort that quietly falls short.
std::optional<float> grenade_launch_pitch(float horizontal_distance);

}  // namespace game
