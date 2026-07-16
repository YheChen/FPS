#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/scene/transform.h"

// Deliberately small entity store: an entity is a generational ID into a
// slot array; components are plain members of Entity (composition, no
// generic ECS - see docs/architecture.md). Grow this only when the game
// demands it.
namespace eng {

struct EntityId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    bool operator==(const EntityId&) const = default;
};

struct Entity {
    std::string name;
    Transform transform;
    // Index into the owning model's mesh list; -1 = no visual (marker).
    int mesh = -1;
    glm::vec4 tint{1.0f};
    bool visible = true;
};

class Scene {
public:
    EntityId create(std::string name);

    // Destroying invalidates the ID (generation bump); the slot is reused.
    void destroy(EntityId id);

    bool alive(EntityId id) const;

    // nullptr if the ID is stale or invalid.
    Entity* get(EntityId id);
    const Entity* get(EntityId id) const;

    std::size_t count() const { return alive_count_; }

    // Iterates alive entities in slot order.
    template <typename F>
    void each(F&& fn) {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.alive) {
                fn(EntityId{i, slot.generation}, slot.entity);
            }
        }
    }

private:
    struct Slot {
        Entity entity;
        std::uint32_t generation = 0;
        bool alive = false;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t alive_count_ = 0;
};

}  // namespace eng
