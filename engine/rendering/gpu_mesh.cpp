#include "engine/rendering/gpu_mesh.h"

#include "engine/rendering/gl.h"

#include <cstddef>
#include <utility>

#include "engine/core/assert.h"

namespace eng {

GpuMesh GpuMesh::upload(const MeshData& mesh) {
    ENG_ASSERT(!mesh.vertices.empty() && !mesh.indices.empty(), "cannot upload an empty mesh");

    GpuMesh gpu;
    glGenVertexArrays(1, &gpu.vao_);
    glGenBuffers(1, &gpu.vbo_);
    glGenBuffers(1, &gpu.ebo_);

    glBindVertexArray(gpu.vao_);

    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(Vertex)),
                 mesh.vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
                 mesh.indices.data(), GL_STATIC_DRAW);

    const auto stride = static_cast<GLsizei>(sizeof(Vertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex, uv)));

    glBindVertexArray(0);

    gpu.index_count_ = static_cast<std::uint32_t>(mesh.indices.size());
    return gpu;
}

void GpuMesh::destroy() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        glDeleteBuffers(1, &ebo_);
        vao_ = vbo_ = ebo_ = 0;
    }
}

GpuMesh::~GpuMesh() {
    destroy();
}

GpuMesh::GpuMesh(GpuMesh&& other) noexcept
    : vao_(std::exchange(other.vao_, 0u)),
      vbo_(std::exchange(other.vbo_, 0u)),
      ebo_(std::exchange(other.ebo_, 0u)),
      index_count_(std::exchange(other.index_count_, 0u)) {}

GpuMesh& GpuMesh::operator=(GpuMesh&& other) noexcept {
    if (this != &other) {
        destroy();
        vao_ = std::exchange(other.vao_, 0u);
        vbo_ = std::exchange(other.vbo_, 0u);
        ebo_ = std::exchange(other.ebo_, 0u);
        index_count_ = std::exchange(other.index_count_, 0u);
    }
    return *this;
}

void GpuMesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count_), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

}  // namespace eng
