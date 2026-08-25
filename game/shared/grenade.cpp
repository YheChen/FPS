#include "game/shared/grenade.h"

#include "game/shared/hitscan.h"

namespace game {

glm::vec3 grenade_throw_velocity(float yaw, float pitch, const glm::vec3& thrower_velocity) {
    // Inherits the thrower's motion, so a grenade thrown while running does
    // not hang in the air behind them. Only the horizontal part: inheriting a
    // fall would make a grenade thrown off a ledge drop like a stone, which
    // reads as the throw failing rather than as physics.
    const glm::vec3 aim = view_direction(yaw, pitch);
    return aim * kGrenadeThrowSpeed + glm::vec3{thrower_velocity.x, 0.0f, thrower_velocity.z};
}

}  // namespace game
