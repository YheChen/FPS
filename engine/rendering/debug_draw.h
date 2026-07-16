#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/shader.h"

namespace eng {

// Immediate-mode world-space line renderer for debugging (axes, rays,
// collision shapes, network visualizations). Accumulate lines during the
// frame, then flush() once with the camera's view-projection.
class DebugDraw {
public:
    static std::optional<DebugDraw> create();

    ~DebugDraw();
    DebugDraw(DebugDraw&& other) noexcept;
    DebugDraw& operator=(DebugDraw&& other) noexcept;
    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    void line(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color);

    // XYZ basis at a transform: X red, Y green, Z blue.
    void axes(const glm::mat4& transform, float length);

    // Axis-aligned wireframe box.
    void aabb(const glm::vec3& min, const glm::vec3& max, const glm::vec3& color);

    // Draws all accumulated lines (one draw call) and clears the buffer.
    // Returns the number of lines drawn.
    std::size_t flush(const glm::mat4& view_projection);

private:
    DebugDraw() = default;
    void destroy();

    struct LineVertex {
        glm::vec3 position;
        glm::vec3 color;
    };

    std::vector<LineVertex> vertices_;
    std::optional<Shader> shader_;
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
    std::size_t vbo_capacity_ = 0;
};

}  // namespace eng
