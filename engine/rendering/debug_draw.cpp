#include "engine/rendering/debug_draw.h"

#include "engine/rendering/gl.h"

#include <array>
#include <cstddef>
#include <utility>

namespace eng {

namespace {

// Shaders omit #version; Shader::create prepends the platform preamble.
constexpr std::string_view kVertexSource = R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;
uniform mat4 u_view_projection;
out vec3 v_color;
void main() {
    v_color = a_color;
    gl_Position = u_view_projection * vec4(a_position, 1.0);
}
)";

constexpr std::string_view kFragmentSource = R"(
in vec3 v_color;
out vec4 o_color;
void main() {
    o_color = vec4(v_color, 1.0);
}
)";

}  // namespace

std::optional<DebugDraw> DebugDraw::create() {
    auto shader = Shader::create("debug_draw", kVertexSource, kFragmentSource);
    if (!shader) {
        return std::nullopt;
    }

    DebugDraw draw;
    draw.shader_ = std::move(shader);
    glGenVertexArrays(1, &draw.vao_);
    glGenBuffers(1, &draw.vbo_);

    glBindVertexArray(draw.vao_);
    glBindBuffer(GL_ARRAY_BUFFER, draw.vbo_);
    const auto stride = static_cast<GLsizei>(sizeof(LineVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(LineVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(LineVertex, color)));
    glBindVertexArray(0);
    return draw;
}

void DebugDraw::destroy() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        vao_ = vbo_ = 0;
    }
}

DebugDraw::~DebugDraw() {
    destroy();
}

DebugDraw::DebugDraw(DebugDraw&& other) noexcept
    : vertices_(std::move(other.vertices_)),
      shader_(std::move(other.shader_)),
      vao_(std::exchange(other.vao_, 0u)),
      vbo_(std::exchange(other.vbo_, 0u)),
      vbo_capacity_(std::exchange(other.vbo_capacity_, 0u)) {}

DebugDraw& DebugDraw::operator=(DebugDraw&& other) noexcept {
    if (this != &other) {
        destroy();
        vertices_ = std::move(other.vertices_);
        shader_ = std::move(other.shader_);
        vao_ = std::exchange(other.vao_, 0u);
        vbo_ = std::exchange(other.vbo_, 0u);
        vbo_capacity_ = std::exchange(other.vbo_capacity_, 0u);
    }
    return *this;
}

void DebugDraw::line(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color) {
    vertices_.push_back({from, color});
    vertices_.push_back({to, color});
}

void DebugDraw::axes(const glm::mat4& transform, float length) {
    const glm::vec3 origin = glm::vec3(transform[3]);
    line(origin, origin + glm::vec3(transform[0]) * length, {1.0f, 0.2f, 0.2f});
    line(origin, origin + glm::vec3(transform[1]) * length, {0.2f, 1.0f, 0.2f});
    line(origin, origin + glm::vec3(transform[2]) * length, {0.2f, 0.4f, 1.0f});
}

void DebugDraw::aabb(const glm::vec3& min, const glm::vec3& max, const glm::vec3& color) {
    const std::array<glm::vec3, 8> c = {{
        {min.x, min.y, min.z},
        {max.x, min.y, min.z},
        {max.x, min.y, max.z},
        {min.x, min.y, max.z},
        {min.x, max.y, min.z},
        {max.x, max.y, min.z},
        {max.x, max.y, max.z},
        {min.x, max.y, max.z},
    }};
    constexpr std::array<std::pair<int, int>, 12> edges = {{
        {0, 1},
        {1, 2},
        {2, 3},
        {3, 0},  // bottom
        {4, 5},
        {5, 6},
        {6, 7},
        {7, 4},  // top
        {0, 4},
        {1, 5},
        {2, 6},
        {3, 7},  // verticals
    }};
    for (const auto& [a, b] : edges) {
        line(c[static_cast<std::size_t>(a)], c[static_cast<std::size_t>(b)], color);
    }
}

std::size_t DebugDraw::flush(const glm::mat4& view_projection) {
    if (vertices_.empty()) {
        return 0;
    }
    const std::size_t line_count = vertices_.size() / 2;

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const std::size_t bytes = vertices_.size() * sizeof(LineVertex);
    if (bytes > vbo_capacity_) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), vertices_.data(),
                     GL_DYNAMIC_DRAW);
        vbo_capacity_ = bytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), vertices_.data());
    }

    shader_->bind();
    shader_->set_mat4("u_view_projection", view_projection);
    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices_.size()));
    glBindVertexArray(0);

    vertices_.clear();
    return line_count;
}

}  // namespace eng
