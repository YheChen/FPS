#include "engine/physics/character_controller.h"

#include "engine/physics/physics_internal.h"

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

#include <cfloat>

#include "engine/core/assert.h"

namespace eng {

namespace {

// Capsule centered at half height, so the character's position is its feet.
JPH::RefConst<JPH::Shape> make_capsule(float height, float radius) {
    const float cylinder_half_height = (height - 2.0f * radius) * 0.5f;
    ENG_ASSERT(cylinder_half_height > 0.0f, "height must exceed twice the radius");
    return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, height * 0.5f, 0.0f),
                                               JPH::Quat::sIdentity(),
                                               new JPH::CapsuleShape(cylinder_half_height, radius))
        .Create()
        .Get();
}

}  // namespace

struct CharacterController::Impl {
    JPH::Ref<JPH::CharacterVirtual> character;
    JPH::RefConst<JPH::Shape> standing;
    JPH::RefConst<JPH::Shape> crouching;
};

CharacterController::CharacterController(PhysicsWorld& world, const glm::vec3& feet_position,
                                         const CharacterConfig& config)
    : impl_(std::make_unique<Impl>()), config_(config) {
    impl_->standing = make_capsule(config.height, config.radius);
    impl_->crouching = make_capsule(config.crouch_height, config.radius);
    const JPH::RefConst<JPH::Shape> shape = impl_->standing;

    JPH::CharacterVirtualSettings settings;
    settings.mShape = shape;
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(config.max_slope_degrees);
    // Accept ground contacts on the lower hemisphere.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -config.radius);

    impl_->character = new JPH::CharacterVirtual(&settings, JPH::RVec3(to_jolt(feet_position)),
                                                 JPH::Quat::sIdentity(), 0, &world.impl().system);
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
    return impl_->character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

glm::vec3 CharacterController::ground_normal() const {
    return from_jolt(impl_->character->GetGroundNormal());
}

void CharacterController::set_crouched(PhysicsWorld& world, bool crouched) {
    if (crouched == crouched_) {
        return;
    }
    world.optimize();
    JPH::PhysicsSystem& system = world.impl().system;
    // FLT_MAX max-penetration: the swap always succeeds. Refusing to stand up
    // under a ceiling is game logic (a headroom check in game/shared), not a
    // physics failure, which keeps this call deterministic.
    impl_->character->SetShape(
        crouched ? impl_->crouching : impl_->standing, FLT_MAX,
        system.GetDefaultBroadPhaseLayerFilter(phys_internal::layers::kMoving),
        system.GetDefaultLayerFilter(phys_internal::layers::kMoving), {}, {},
        world.impl().temp_allocator);
    crouched_ = crouched;
}

void CharacterController::refresh_ground(PhysicsWorld& world) {
    world.optimize();
    JPH::PhysicsSystem& system = world.impl().system;
    impl_->character->RefreshContacts(
        system.GetDefaultBroadPhaseLayerFilter(phys_internal::layers::kMoving),
        system.GetDefaultLayerFilter(phys_internal::layers::kMoving), {}, {},
        world.impl().temp_allocator);
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
