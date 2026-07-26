#pragma once

#include <cstdint>
#include <optional>

namespace eng {

// Off-screen colour target with an optional depth attachment. Move-only.
//
// Colour format is chosen by `hdr`: RGBA16F when true, RGBA8 otherwise.
// HDR is *requested*, not guaranteed -- see create().
class RenderTarget {
public:
    // Attempts an RGBA16F target when `hdr` is set and silently falls back to
    // RGBA8 if the framebuffer does not come out complete. That fallback is
    // the point: WebGL 2 only supports float colour attachments with
    // EXT_color_buffer_float, and probing the framebuffer is more reliable
    // than parsing extension strings (a driver can advertise the extension
    // and still refuse the attachment). Ask hdr() afterwards for what you
    // actually got.
    //
    // Returns nullopt only when even the RGBA8 fallback fails.
    static std::optional<RenderTarget> create(int width, int height, bool hdr, bool with_depth);

    ~RenderTarget();
    RenderTarget(RenderTarget&& other) noexcept;
    RenderTarget& operator=(RenderTarget&& other) noexcept;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // Binds the framebuffer and sets the viewport to cover it.
    void bind() const;

    void bind_color(std::uint32_t unit) const;

    int width() const { return width_; }
    int height() const { return height_; }
    bool hdr() const { return hdr_; }

private:
    RenderTarget() = default;

    std::uint32_t framebuffer_ = 0;
    std::uint32_t color_texture_ = 0;
    std::uint32_t depth_renderbuffer_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool hdr_ = false;
};

}  // namespace eng
