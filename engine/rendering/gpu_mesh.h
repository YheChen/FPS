#pragma once

#include <cstdint>

#include "engine/rendering/mesh_data.h"

namespace eng {

// GPU copy of a MeshData: VAO + vertex/index buffers, RAII, move-only.
// Attribute layout: 0 = position (vec3), 1 = normal (vec3), 2 = uv (vec2).
class GpuMesh {
public:
    static GpuMesh upload(const MeshData& mesh);

    ~GpuMesh();
    GpuMesh(GpuMesh&& other) noexcept;
    GpuMesh& operator=(GpuMesh&& other) noexcept;
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    // Binds the VAO and issues one indexed draw call.
    void draw() const;

    std::uint32_t index_count() const { return index_count_; }
    std::uint32_t triangle_count() const { return index_count_ / 3; }

private:
    GpuMesh() = default;
    void destroy();

    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
    std::uint32_t ebo_ = 0;
    std::uint32_t index_count_ = 0;
};

}  // namespace eng
