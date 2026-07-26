#include "engine/rendering/joint_texture.h"

#include "engine/rendering/gl.h"

#include <algorithm>
#include <utility>

#include "engine/core/log.h"

namespace eng {

std::optional<JointTexture> JointTexture::create(std::size_t max_joints) {
    if (max_joints == 0) {
        log::error("JointTexture: capacity must be positive");
        return std::nullopt;
    }

    JointTexture texture;
    texture.capacity_ = max_joints;

    glGenTextures(1, &texture.texture_);
    glBindTexture(GL_TEXTURE_2D, texture.texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, static_cast<GLsizei>(max_joints), 0, GL_RGBA,
                 GL_FLOAT, nullptr);
    // NEAREST and no mips: the shader uses texelFetch, so no filtering is
    // involved. That also keeps this inside core WebGL 2, which does not
    // support linear filtering of float textures without an extension.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    log::info("Joint texture: {} joints", max_joints);
    return texture;
}

JointTexture::~JointTexture() {
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
    }
}

JointTexture::JointTexture(JointTexture&& other) noexcept
    : texture_(std::exchange(other.texture_, 0u)), capacity_(other.capacity_) {}

JointTexture& JointTexture::operator=(JointTexture&& other) noexcept {
    if (this != &other) {
        if (texture_ != 0) {
            glDeleteTextures(1, &texture_);
        }
        texture_ = std::exchange(other.texture_, 0u);
        capacity_ = other.capacity_;
    }
    return *this;
}

void JointTexture::upload_and_bind(std::span<const glm::mat4> matrices, std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (matrices.empty()) {
        return;
    }
    const auto rows = static_cast<GLsizei>(std::min(matrices.size(), capacity_));
    // A mat4 is 16 contiguous floats in column-major order, which is exactly
    // four RGBA texels, so the matrices upload without any repacking.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, rows, GL_RGBA, GL_FLOAT, matrices.data());
}

}  // namespace eng
