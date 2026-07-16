#pragma once

#include <cstdint>
#include <deque>

#include <glm/glm.hpp>

#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
#include "game/shared/input_command.h"
#include "game/shared/player_movement.h"

// Client-side prediction with server reconciliation (ADR 0002). Lives in
// game/shared so it can be unit-tested headlessly.
//
// Flow per tick:   tick(cmd)            - simulate locally, remember cmd
// Per snapshot:    reconcile(server...) - rewind to the server's state and
//                  replay every input the server has not yet processed.
// The visual correction is exposed as a decaying smoothing offset so
// corrections never snap the camera.
namespace game {

class Prediction {
public:
    Prediction(eng::PhysicsWorld& world, const glm::vec3& spawn);

    const PlayerState& state() const { return state_; }
    const PlayerState& previous_state() const { return previous_state_; }

    // Predict one fixed tick locally and store the command for replay.
    void tick(const InputCommand& command);

    struct ReconcileResult {
        float error_meters = 0.0f;  // |predicted - replayed| at the newest tick
        bool corrected = false;
        std::size_t replayed = 0;
    };

    // Applies an authoritative state acked at `last_processed_seq`.
    ReconcileResult reconcile(const glm::vec3& position, const glm::vec3& velocity,
                              bool on_ground, std::uint32_t last_processed_seq);

    // Commands the server has not acknowledged yet (oldest first).
    const std::deque<InputCommand>& pending() const { return pending_; }

    // Visual smoothing: add to the rendered position; decays to zero.
    glm::vec3 smoothing_offset() const { return smoothing_offset_; }
    void update_smoothing(float dt);

    // Hard reset (respawn/teleport): clears pending inputs and smoothing.
    void reset(const glm::vec3& position);

    static constexpr std::size_t kMaxPending = 128;
    static constexpr float kSnapThresholdMeters = 2.0f;

private:
    eng::PhysicsWorld* world_;
    eng::CharacterController controller_;
    PlayerState state_{};
    PlayerState previous_state_{};
    std::deque<InputCommand> pending_;
    glm::vec3 smoothing_offset_{0.0f};
};

}  // namespace game
