#include "engine/scene/scene.h"

#include <utility>

namespace eng {

EntityId Scene::create(std::string name) {
    std::uint32_t index = 0;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    Slot& slot = slots_[index];
    slot.entity = Entity{};
    slot.entity.name = std::move(name);
    slot.alive = true;
    ++alive_count_;
    return {index, slot.generation};
}

void Scene::destroy(EntityId id) {
    if (!alive(id)) {
        return;
    }
    Slot& slot = slots_[id.index];
    slot.alive = false;
    ++slot.generation;  // stale IDs now fail alive()
    free_.push_back(id.index);
    --alive_count_;
}

bool Scene::alive(EntityId id) const {
    return id.index < slots_.size() && slots_[id.index].alive &&
           slots_[id.index].generation == id.generation;
}

Entity* Scene::get(EntityId id) {
    return alive(id) ? &slots_[id.index].entity : nullptr;
}

const Entity* Scene::get(EntityId id) const {
    return alive(id) ? &slots_[id.index].entity : nullptr;
}

}  // namespace eng
