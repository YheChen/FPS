#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/mesh_data.h"

// Maps built in memory out of boxes, the way tools/gen_arena.py builds the
// real ones: each box is a unit cube under a translate-rotate-scale node, and
// the server (or a test standing in for it) walks the nodes and collects
// (mesh, world transform) pairs. What these helpers call a wall is exactly
// what the physics world and the navigation grid would be handed.
namespace test {

using BoxMeshes = std::vector<std::pair<eng::MeshData, glm::mat4>>;

inline std::pair<eng::MeshData, glm::mat4> box(const glm::vec3& center, const glm::vec3& size,
                                               float rotation_z_degrees = 0.0f) {
    glm::mat4 transform = glm::translate(glm::mat4{1.0f}, center);
    if (rotation_z_degrees != 0.0f) {
        transform = glm::rotate(transform, glm::radians(rotation_z_degrees), {0.0f, 0.0f, 1.0f});
    }
    transform = glm::scale(transform, size);
    return {eng::MeshData::unit_cube(), transform};
}

// A floor whose top face is y = 0, `half` metres to each side of the origin.
inline BoxMeshes floor(float half) {
    return {box({0.0f, -0.5f, 0.0f}, {2.0f * half, 1.0f, 2.0f * half})};
}

}  // namespace test
