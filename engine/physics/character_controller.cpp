#include "engine/physics/character_controller.h"

#include "engine/physics/physics_internal.h"

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

#include "engine/core/assert.h"

namespace eng {

struct CharacterController::Impl {
    JPH::Ref<JPH::CharacterVirtual> character;
};

CharacterController::CharacterController(PhysicsWorld& world, const glm::vec3& feet_position,
                                         const CharacterConfig& config)
    : impl_(std::make_unique<Impl>()), config_(config) {
    const float cylinder_half_height = (config.height - 2.0f * config.radius) * 0.5f;
    ENG_ASSERT(cylinder_half_height > 0.0f, "height must exceed twice the radius");

    // Capsule centered at half height so the character position is the feet.
    const JPH::RefConst<JPH::Shape> shape =
        JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(0.0f, config.height * 0.5f, 0.0f), JPH::Quat::sIdentity(),
            new JPH::CapsuleShape(cylinder_half_height, config.radius))
            .Create()
            .Get();

    JPH::CharacterVirtualSettings settings;
    settings.mShape = shape;
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(config.max_slope_degrees);
    // Accept ground contacts on the lower hemisphere.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -config.radius);

    impl_->character = new JPH::CharacterVirtual(&settings, JPH::RVec3(to_jolt(feet_position)),
                                                 JPH::Quat::sIdentity(), 0,
                                                 &world.impl().system);
}

CharacterController::~CharacterController() = default;
CharacterController::CharacterController(CharacterController&&) noexcept = default;
CharacterController& CharacterController::operator=(CharacterController&&) noexcept = default;

glm::vec3 CharacterController::position() const {
    return from_jolt(JPH::Vec3(impl_->character->GetPosition()));
}

void CharacterController::set_position(const glm::vec3& feet_position) {
    impl_->character->SetPosition(JPH::RVec3(to_jolt(feet_position)));
}

glm::vec3 CharacterController::velocity() const {
    return from_jolt(impl_->character->GetLinearVelocity());
}

void CharacterController::set_velocity(const glm::vec3& velocity) {
    impl_->character->SetLinearVelocity(to_jolt(velocity));
}

bool CharacterController::on_ground() const {
    return impl_->character->GetGroundState() ==
           JPH::CharacterBase::EGroundState::OnGround;
}

glm::vec3 CharacterController::ground_normal() const {
    return from_jolt(impl_->character->GetGroundNormal());
}

void CharacterController::update(PhysicsWorld& world, float dt, const glm::vec3& gravity) {
    world.optimize();  // ensure broadphase is valid before queries

    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -0.4f, 0.0f);
    settings.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.4f, 0.0f);

    JPH::PhysicsSystem& system = world.impl().system;
    impl_->character->ExtendedUpdate(
        dt, to_jolt(gravity), settings,
        system.GetDefaultBroadPhaseLayerFilter(phys_internal::layers::kMoving),
        system.GetDefaultLayerFilter(phys_internal::layers::kMoving), {}, {},
        world.impl().temp_allocator);
}

}  // namespace eng
