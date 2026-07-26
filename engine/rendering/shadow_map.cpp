#include "engine/rendering/shadow_map.h"

#include "engine/rendering/gl.h"

#include <utility>

#include "engine/core/assert.h"
#include "engine/core/log.h"

namespace eng {

std::optional<ShadowMap> ShadowMap::create(int resolution) {
    ENG_ASSERT(resolution > 0, "shadow map resolution must be positive");

    ShadowMap map;
    map.resolution_ = resolution;

    glGenTextures(1, &map.depth_texture_);
    glBindTexture(GL_TEXTURE_2D, map.depth_texture_);
    // DEPTH_COMPONENT24 with UNSIGNED_INT is valid on both desktop GL 4.1
    // and GLES 3.0 / WebGL 2.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, resolution, resolution, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamp to a border of "fully lit": anything outside the light's
    // frustum must not be reported as shadowed. GLES has no border color,
    // so clamp to edge and rely on the shader's range check instead.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Hardware comparison: sampling returns the filtered pass/fail result,
    // which gives 2x2 PCF for free on top of whatever the shader does.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &map.framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, map.framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, map.depth_texture_,
                           0);
    // Depth only: no color attachment, so the draw/read buffers must be
    // switched off or the framebuffer is incomplete on desktop GL.
#if defined(__EMSCRIPTEN__)
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
#else
    glDrawBuffer(GL_NONE);
#endif
    glReadBuffer(GL_NONE);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log::error("Shadow map framebuffer incomplete (0x{:04x})", status);
        return std::nullopt;
    }
    log::info("Shadow map: {}x{} depth target", resolution, resolution);
    return map;
}

ShadowMap::~ShadowMap() {
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
    }
    if (depth_texture_ != 0) {
        glDeleteTextures(1, &depth_texture_);
    }
}

ShadowMap::ShadowMap(ShadowMap&& other) noexcept
    : framebuffer_(std::exchange(other.framebuffer_, 0u)),
      depth_texture_(std::exchange(other.depth_texture_, 0u)),
      resolution_(other.resolution_) {}

ShadowMap& ShadowMap::operator=(ShadowMap&& other) noexcept {
    if (this != &other) {
        if (framebuffer_ != 0) {
            glDeleteFramebuffers(1, &framebuffer_);
        }
        if (depth_texture_ != 0) {
            glDeleteTextures(1, &depth_texture_);
        }
        framebuffer_ = std::exchange(other.framebuffer_, 0u);
        depth_texture_ = std::exchange(other.depth_texture_, 0u);
        resolution_ = other.resolution_;
    }
    return *this;
}

void ShadowMap::begin_depth_pass() const {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, resolution_, resolution_);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void ShadowMap::end_depth_pass(int viewport_width, int viewport_height) const {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewport_width, viewport_height);
}

void ShadowMap::bind_depth(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, depth_texture_);
}

}  // namespace eng
