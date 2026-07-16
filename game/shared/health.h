#pragma once

namespace game {

struct Health {
    float current = 100.0f;
    float max = 100.0f;

    bool alive() const { return current > 0.0f; }
};

// Applies damage; returns true exactly when this call caused death.
// Damage on an already-dead target is ignored.
inline bool apply_damage(Health& health, float amount) {
    if (!health.alive() || amount <= 0.0f) {
        return false;
    }
    health.current -= amount;
    if (health.current <= 0.0f) {
        health.current = 0.0f;
        return true;
    }
    return false;
}

inline void reset_health(Health& health) {
    health.current = health.max;
}

}  // namespace game
