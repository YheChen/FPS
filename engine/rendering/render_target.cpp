#include "engine/rendering/render_target.h"

#include "engine/rendering/gl.h"

#include <utility>

#include "engine/core/assert.h"
#include "engine/core/log.h"

namespace eng {

namespace {

// Builds the attachments for one attempt at a given colour format. Returns
// false (after cleaning up) if the framebuffer is not complete.
bool try_build(std::uint32_t& framebuffer, std::uint32_t& color_texture,
               std::uint32_t& depth_renderbuffer, int width, int height, bool hdr,
               bool with_depth) {
    glGenTextures(1, &color_texture);
    glBindTexture(GL_TEXTURE_2D, color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, width, height, 0, GL_RGBA,
                 hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    // Linear filtering matters: the bloom blur samples between texels, and
    // clamp-to-edge stops the blur from wrapping bright pixels around the
    // screen.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);

    if (with_depth) {
        glGenRenderbuffers(1, &depth_renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                  depth_renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status == GL_FRAMEBUFFER_COMPLETE) {
        return true;
    }

    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &color_texture);
    if (depth_renderbuffer != 0) {
        glDeleteRenderbuffers(1, &depth_renderbuffer);
    }
    framebuffer = 0;
    color_texture = 0;
    depth_renderbuffer = 0;
    return false;
}

}  // namespace

std::optional<RenderTarget> RenderTarget::create(int width, int height, bool hdr, bool with_depth) {
    ENG_ASSERT(width > 0 && height > 0, "render target size must be positive");

    RenderTarget target;
    target.width_ = width;
    target.height_ = height;

    if (hdr && try_build(target.framebuffer_, target.color_texture_, target.depth_renderbuffer_,
                         width, height, true, with_depth)) {
        target.hdr_ = true;
        return target;
    }
    if (hdr) {
        log::warn(
            "RenderTarget: RGBA16F unavailable at {}x{}; falling back to RGBA8 "
            "(bloom will clip at 1.0)",
            width, height);
    }
    if (try_build(target.framebuffer_, target.color_texture_, target.depth_renderbuffer_, width,
                  height, false, with_depth)) {
        target.hdr_ = false;
        return target;
    }

    log::error("RenderTarget: could not create a complete framebuffer at {}x{}", width, height);
    return std::nullopt;
}

RenderTarget::~RenderTarget() {
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
    }
    if (color_texture_ != 0) {
        glDeleteTextures(1, &color_texture_);
    }
    if (depth_renderbuffer_ != 0) {
        glDeleteRenderbuffers(1, &depth_renderbuffer_);
    }
}

RenderTarget::RenderTarget(RenderTarget&& other) noexcept
    : framebuffer_(std::exchange(other.framebuffer_, 0u)),
      color_texture_(std::exchange(other.color_texture_, 0u)),
      depth_renderbuffer_(std::exchange(other.depth_renderbuffer_, 0u)),
      width_(other.width_),
      height_(other.height_),
      hdr_(other.hdr_) {}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept {
    if (this != &other) {
        if (framebuffer_ != 0) {
            glDeleteFramebuffers(1, &framebuffer_);
        }
        if (color_texture_ != 0) {
            glDeleteTextures(1, &color_texture_);
        }
        if (depth_renderbuffer_ != 0) {
            glDeleteRenderbuffers(1, &depth_renderbuffer_);
        }
        framebuffer_ = std::exchange(other.framebuffer_, 0u);
        color_texture_ = std::exchange(other.color_texture_, 0u);
        depth_renderbuffer_ = std::exchange(other.depth_renderbuffer_, 0u);
        width_ = other.width_;
        height_ = other.height_;
        hdr_ = other.hdr_;
    }
    return *this;
}

void RenderTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width_, height_);
}

void RenderTarget::bind_color(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, color_texture_);
}

}  // namespace eng
