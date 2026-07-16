#pragma once

// Shared internals for engine/physics translation units ONLY. Never include
// from outside engine/physics/.

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <vector>

#include "engine/physics/physics_world.h"

namespace eng::phys_internal {

// Object layers.
namespace layers {
constexpr JPH::ObjectLayer kNonMoving = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::uint kCount = 2;
}  // namespace layers

// Broadphase layers.
namespace bp_layers {
constexpr JPH::BroadPhaseLayer kNonMoving{0};
constexpr JPH::BroadPhaseLayer kMoving{1};
constexpr JPH::uint kCount = 2;
}  // namespace bp_layers

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return bp_layers::kCount; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == layers::kNonMoving ? bp_layers::kNonMoving : bp_layers::kMoving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp_layers::kNonMoving ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer object_layer,
                       JPH::BroadPhaseLayer broadphase_layer) const override {
        if (object_layer == layers::kNonMoving) {
            return broadphase_layer == bp_layers::kMoving;  // static vs moving only
        }
        return true;  // moving collides with everything
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == layers::kNonMoving) {
            return b == layers::kMoving;
        }
        return true;
    }
};

// Registers Jolt's allocator/factory/types exactly once per process.
void ensure_jolt_initialized();

}  // namespace eng::phys_internal

namespace eng {

struct PhysicsWorld::Impl {
    phys_internal::BroadPhaseLayerMap bp_layer_map;
    phys_internal::ObjectVsBroadPhaseFilter ovb_filter;
    phys_internal::ObjectLayerPairFilterImpl olp_filter;
    JPH::TempAllocatorImpl temp_allocator{4 * 1024 * 1024};
    JPH::JobSystemSingleThreaded job_system{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem system;
    std::vector<JPH::BodyID> bodies;
    bool broadphase_dirty = false;
};

inline JPH::Vec3 to_jolt(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}

inline glm::vec3 from_jolt(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

}  // namespace eng
