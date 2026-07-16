#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <glm/glm.hpp>

namespace eng {

// RAII wrapper for a linked GL program. Move-only.
// Uniform locations are cached; setting a uniform the shader does not have
// logs one warning and is otherwise a no-op.
class Shader {
public:
    // Compiles and links; on failure logs the info log and returns nullopt.
    // `name` is used only for log messages.
    static std::optional<Shader> create(std::string_view name, std::string_view vertex_source,
                                        std::string_view fragment_source);

    ~Shader();
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void bind() const;

    void set_int(std::string_view name, int value) const;
    void set_float(std::string_view name, float value) const;
    void set_vec3(std::string_view name, const glm::vec3& value) const;
    void set_vec4(std::string_view name, const glm::vec4& value) const;
    void set_mat3(std::string_view name, const glm::mat3& value) const;
    void set_mat4(std::string_view name, const glm::mat4& value) const;

private:
    Shader() = default;

    std::int32_t location(std::string_view name) const;

    std::uint32_t program_ = 0;
    std::string name_;
    mutable std::unordered_map<std::string, std::int32_t> locations_;
};

}  // namespace eng
