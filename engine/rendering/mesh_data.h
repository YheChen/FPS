#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// CPU-side mesh representation. Lives in the headless-safe engine target:
// the server uses it for collision geometry; the client uploads it to the
// GPU via GpuMesh (engine_platform).
namespace eng {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    // Axis-aligned unit cube centered on the origin (1x1x1), 24 vertices
    // (per-face normals), 36 indices, CCW winding when viewed from outside.
    static MeshData unit_cube();
};

}  // namespace eng
