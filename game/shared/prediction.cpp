#include "game/shared/prediction.h"

#include <cmath>

#include "engine/core/log.h"

namespace game {

Prediction::Prediction(eng::PhysicsWorld& world, const glm::vec3& spawn)
    : world_(&world), controller_(world, spawn) {
    state_.position = spawn;
    previous_state_ = state_;
}

void Prediction::tick(const InputCommand& command) {
    previous_state_ = state_;
    advance_player(state_, command, kTickSeconds, controller_, *world_);
    pending_.push_back(command);
    if (pending_.size() > kMaxPending) {
        // Server has been silent for >2 seconds; stop growing.
        pending_.pop_front();
        eng::log::warn("Prediction: pending input overflow (server not acking)");
    }
}

Prediction::ReconcileResult Prediction::reconcile(const glm::vec3& position,
                                                  const glm::vec3& velocity, bool on_ground,
                                                  std::uint32_t last_processed_seq) {
    ReconcileResult result;

    // Drop everything the server has already consumed.
    while (!pending_.empty() && pending_.front().sequence <= last_processed_seq) {
        pending_.pop_front();
    }

    const glm::vec3 predicted_position = state_.position;

    // Rewind to the authoritative state and replay unacked inputs with the
    // exact shared movement code.
    state_.position = position;
    state_.velocity = velocity;
    state_.on_ground = on_ground;
    for (const InputCommand& command : pending_) {
        advance_player(state_, command, kTickSeconds, controller_, *world_);
    }
    result.replayed = pending_.size();
    result.error_meters = glm::length(predicted_position - state_.position);

    if (result.error_meters > 1e-4f) {
        result.corrected = true;
        if (result.error_meters > kSnapThresholdMeters) {
            // Teleport-sized error: snap, do not smooth.
            smoothing_offset_ = {0.0f, 0.0f, 0.0f};
        } else {
            // Carry the visual difference and decay it over ~100 ms.
            smoothing_offset_ += predicted_position - state_.position;
        }
        previous_state_ = state_;
    }
    return result;
}

void Prediction::update_smoothing(float dt) {
    const float decay = std::exp(-15.0f * dt);
    smoothing_offset_ *= decay;
    if (glm::dot(smoothing_offset_, smoothing_offset_) < 1e-8f) {
        smoothing_offset_ = {0.0f, 0.0f, 0.0f};
    }
}

void Prediction::reset(const glm::vec3& position) {
    state_ = {};
    state_.position = position;
    previous_state_ = state_;
    pending_.clear();
    smoothing_offset_ = {0.0f, 0.0f, 0.0f};
    controller_.set_position(position);
    controller_.set_velocity({0.0f, 0.0f, 0.0f});
}

}  // namespace game
