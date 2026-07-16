#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include <glm/glm.hpp>

#include "engine/rendering/mesh_data.h"

// Jolt Physics wrapper. Jolt types never appear in engine public headers
// (pimpl); game and engine code sees only glm math.
//
// Threading: main thread only. Jolt's internal job system is not used
// (JobSystemSingleThreaded) - our worlds are tiny and determinism of a
// single run matters more than parallel stepping.
namespace eng {

struct RayHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Static collision geometry. `transform` is baked into the vertices.
    void add_static_mesh(const MeshData& mesh, const glm::mat4& transform);
    void add_static_box(const glm::vec3& center, const glm::vec3& half_extents);

    // Call once after all static geometry is added (broadphase rebuild).
    void optimize();

    // `direction` must be normalized. Returns the closest hit within
    // max_distance, or nullopt.
    std::optional<RayHit> raycast(const glm::vec3& from, const glm::vec3& direction,
                                  float max_distance) const;

    // Advances dynamic simulation by one fixed step. (Static-only worlds and
    // CharacterVirtual do not strictly need this, but dynamic bodies will.)
    void step(float dt);

    std::size_t body_count() const;

    // Internal access for other engine/physics translation units.
    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace eng
