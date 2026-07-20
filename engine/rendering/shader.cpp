#include "engine/rendering/shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <string>
#include <utility>

#include "engine/core/log.h"
#include "engine/rendering/gl.h"

namespace eng {

namespace {

std::optional<GLuint> compile_stage(std::string_view shader_name, GLenum stage,
                                    std::string_view source) {
    const GLuint shader = glCreateShader(stage);
    // Shader bodies omit the #version line; we prepend the platform preamble
    // (desktop GLSL 410 core vs WebGL2 GLSL ES 300) so one source works on
    // both backends.
    const std::string full = std::string(glsl_preamble()) + std::string(source);
    const GLchar* text = full.c_str();
    const auto length = static_cast<GLint>(full.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        std::array<char, 2048> info{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(info.size()), nullptr, info.data());
        log::error("Shader '{}' {} stage compile failed:\n{}", shader_name,
                   stage == GL_VERTEX_SHADER ? "vertex" : "fragment", info.data());
        glDeleteShader(shader);
        return std::nullopt;
    }
    return shader;
}

}  // namespace

std::optional<Shader> Shader::create(std::string_view name, std::string_view vertex_source,
                                     std::string_view fragment_source) {
    const auto vs = compile_stage(name, GL_VERTEX_SHADER, vertex_source);
    if (!vs) {
        return std::nullopt;
    }
    const auto fs = compile_stage(name, GL_FRAGMENT_SHADER, fragment_source);
    if (!fs) {
        glDeleteShader(*vs);
        return std::nullopt;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, *vs);
    glAttachShader(program, *fs);
    glLinkProgram(program);
    glDeleteShader(*vs);
    glDeleteShader(*fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        std::array<char, 2048> info{};
        glGetProgramInfoLog(program, static_cast<GLsizei>(info.size()), nullptr, info.data());
        log::error("Shader '{}' link failed:\n{}", name, info.data());
        glDeleteProgram(program);
        return std::nullopt;
    }

    Shader shader;
    shader.program_ = program;
    shader.name_ = std::string(name);
    return shader;
}

Shader::~Shader() {
    if (program_ != 0) {
        glDeleteProgram(program_);
    }
}

Shader::Shader(Shader&& other) noexcept
    : program_(std::exchange(other.program_, 0u)),
      name_(std::move(other.name_)),
      locations_(std::move(other.locations_)) {}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (program_ != 0) {
            glDeleteProgram(program_);
        }
        program_ = std::exchange(other.program_, 0u);
        name_ = std::move(other.name_);
        locations_ = std::move(other.locations_);
    }
    return *this;
}

void Shader::bind() const {
    glUseProgram(program_);
}

std::int32_t Shader::location(std::string_view name) const {
    const auto it = locations_.find(std::string(name));
    if (it != locations_.end()) {
        return it->second;
    }
    const GLint loc = glGetUniformLocation(program_, std::string(name).c_str());
    if (loc < 0) {
        log::warn("Shader '{}': uniform '{}' not found (optimized out or typo)", name_, name);
    }
    locations_.emplace(std::string(name), loc);
    return loc;
}

void Shader::set_int(std::string_view name, int value) const {
    glUniform1i(location(name), value);
}

void Shader::set_float(std::string_view name, float value) const {
    glUniform1f(location(name), value);
}

void Shader::set_vec3(std::string_view name, const glm::vec3& value) const {
    glUniform3fv(location(name), 1, glm::value_ptr(value));
}

void Shader::set_vec4(std::string_view name, const glm::vec4& value) const {
    glUniform4fv(location(name), 1, glm::value_ptr(value));
}

void Shader::set_mat3(std::string_view name, const glm::mat3& value) const {
    glUniformMatrix3fv(location(name), 1, GL_FALSE, glm::value_ptr(value));
}

void Shader::set_mat4(std::string_view name, const glm::mat4& value) const {
    glUniformMatrix4fv(location(name), 1, GL_FALSE, glm::value_ptr(value));
}

}  // namespace eng
