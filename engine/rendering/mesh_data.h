#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// CPU-side mesh representation. Lives in the headless-safe engine target:
// the server uses it for collision geometry; the client uploads it to the
// GPU via GpuMesh (engine_platform).
namespace eng {

// One vertex format for everything, skinned or not. The skinning attributes
// cost 32 bytes on static geometry that never uses them, which is worth it
// to avoid a second vertex layout, a second GpuMesh path and a second
// shader family: the arena is a few hundred vertices, so the memory is
// noise.
//
// An unskinned vertex has all weights at 0, which the shader treats as
// "use the model matrix unchanged".
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::uvec4 joints{0u};    // indices into the skin's joint list
    glm::vec4 weights{0.0f};  // parallel to `joints`, sums to 1 when skinned

    bool skinned() const { return weights.x + weights.y + weights.z + weights.w > 0.0f; }
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    // Axis-aligned unit cube centered on the origin (1x1x1), 24 vertices
    // (per-face normals), 36 indices, CCW winding when viewed from outside.
    static MeshData unit_cube();
};

}  // namespace eng
