#include "engine/physics/physics_world.h"

#include "engine/physics/physics_internal.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/RegisterTypes.h>

#include <cmath>
#include <mutex>

#include "engine/core/assert.h"
#include "engine/core/log.h"

namespace eng {

namespace phys_internal {

void ensure_jolt_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

}  // namespace phys_internal

using phys_internal::layers::kMoving;
using phys_internal::layers::kNonMoving;

PhysicsWorld::PhysicsWorld() {
    // Jolt's global allocator/factory MUST exist before Impl is constructed:
    // Impl's TempAllocatorImpl allocates through JPH::Allocate.
    phys_internal::ensure_jolt_initialized();
    impl_ = std::make_unique<Impl>();
    constexpr JPH::uint kMaxBodies = 2048;
    constexpr JPH::uint kBodyMutexes = 0;  // default
    constexpr JPH::uint kMaxBodyPairs = 2048;
    constexpr JPH::uint kMaxContacts = 2048;
    impl_->system.Init(kMaxBodies, kBodyMutexes, kMaxBodyPairs, kMaxContacts, impl_->bp_layer_map,
                       impl_->ovb_filter, impl_->olp_filter);
    impl_->system.SetGravity(JPH::Vec3(0.0f, -20.0f, 0.0f));
}

PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

void PhysicsWorld::add_static_mesh(const MeshData& mesh, const glm::mat4& transform) {
    ENG_ASSERT(mesh.indices.size() % 3 == 0, "static mesh must be triangles");

    JPH::VertexList vertices;
    vertices.reserve(mesh.vertices.size());
    for (const Vertex& v : mesh.vertices) {
        const glm::vec3 world = glm::vec3(transform * glm::vec4(v.position, 1.0f));
        vertices.push_back(JPH::Float3(world.x, world.y, world.z));
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        triangles.push_back(
            JPH::IndexedTriangle(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]));
    }

    JPH::MeshShapeSettings settings(vertices, triangles);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        log::error("Jolt mesh shape creation failed: {}", result.GetError().c_str());
        return;
    }

    JPH::BodyCreationSettings body_settings(result.Get(), JPH::RVec3::sZero(),
                                            JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                            kNonMoving);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        body_settings, JPH::EActivation::DontActivate);
    impl_->bodies.push_back(id);
    impl_->broadphase_dirty = true;
}

void PhysicsWorld::add_static_box(const glm::vec3& center, const glm::vec3& half_extents) {
    JPH::BodyCreationSettings body_settings(new JPH::BoxShape(to_jolt(half_extents)),
                                            JPH::RVec3(to_jolt(center)), JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static, kNonMoving);
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        body_settings, JPH::EActivation::DontActivate);
    impl_->bodies.push_back(id);
    impl_->broadphase_dirty = true;
}

void PhysicsWorld::optimize() {
    if (impl_->broadphase_dirty) {
        impl_->system.OptimizeBroadPhase();
        impl_->broadphase_dirty = false;
    }
}

std::optional<RayHit> PhysicsWorld::raycast(const glm::vec3& from, const glm::vec3& direction,
                                            float max_distance) const {
    ENG_ASSERT(std::abs(glm::length(direction) - 1.0f) < 0.01f,
               "raycast direction must be normalized");

    const JPH::RRayCast ray{JPH::RVec3(to_jolt(from)), to_jolt(direction * max_distance)};
    JPH::RayCastResult result;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) {
        return std::nullopt;
    }

    RayHit hit;
    hit.distance = max_distance * result.mFraction;
    hit.position = from + direction * hit.distance;

    const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        hit.normal = from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
            result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
    }
    return hit;
}

void PhysicsWorld::step(float dt) {
    optimize();
    impl_->system.Update(dt, 1, &impl_->temp_allocator, &impl_->job_system);
}

std::size_t PhysicsWorld::body_count() const {
    return impl_->bodies.size();
}

}  // namespace eng
