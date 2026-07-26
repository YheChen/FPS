#pragma once

#include <optional>

#include <glm/glm.hpp>

#include "engine/rendering/render_target.h"
#include "engine/rendering/shader.h"

namespace eng {

// Post-processing chain: HDR scene target -> bloom -> tonemap -> FXAA -> screen.
//
// Usage per frame:
//     postfx.begin_scene();      // bind the HDR target and clear it
//     ... draw the 3D world, particles, debug lines ...
//     postfx.resolve();          // run the chain to the default framebuffer
//     ... draw the HUD / ImGui, which must NOT be tonemapped or blurred ...
//
// Five fullscreen passes at most (bright, blur x2, composite+tonemap, FXAA).
// Bloom runs at half resolution, which is both cheaper and a wider blur for
// the same kernel.
class PostFx {
public:
    struct Settings {
        bool bloom = true;
        bool fxaa = true;
        float exposure = 1.0f;
        float bloom_threshold = 1.0f;  // luminance where bloom starts
        float bloom_knee = 0.35f;      // width of the soft ramp, 0 = hard cut
        float bloom_intensity = 0.55f;
    };

    static std::optional<PostFx> create(int width, int height);

    ~PostFx();
    PostFx(PostFx&& other) noexcept;
    PostFx& operator=(PostFx&& other) noexcept;
    PostFx(const PostFx&) = delete;
    PostFx& operator=(const PostFx&) = delete;

    // Recreates the targets. Cheap enough to call on every resize event, but
    // it does reallocate, so only call it when the size actually changed.
    bool resize(int width, int height);

    // Binds the HDR scene target, clears it, and re-enables the depth test
    // that resolve() turns off.
    void begin_scene(const glm::vec4& clear_color) const;

    // Runs the chain into the default framebuffer, restoring the viewport to
    // the target size. Leaves depth test and blending disabled.
    void resolve(const Settings& settings);

    int width() const { return width_; }
    int height() const { return height_; }
    // False when RGBA16F was unavailable: the scene target clips at 1.0, so
    // bloom only picks up what the tonemap would have kept anyway.
    bool hdr() const;
    // Fullscreen passes run by the last resolve().
    int last_pass_count() const { return last_pass_count_; }

private:
    PostFx() = default;

    void draw_fullscreen() const;

    std::optional<RenderTarget> scene_;    // full res, HDR, with depth
    std::optional<RenderTarget> bloom_a_;  // half res
    std::optional<RenderTarget> bloom_b_;  // half res
    std::optional<RenderTarget> ldr_;      // full res, RGBA8, tonemap output

    std::optional<Shader> bright_;
    std::optional<Shader> blur_;
    std::optional<Shader> composite_;
    std::optional<Shader> fxaa_;

    std::uint32_t empty_vao_ = 0;  // core profile needs a VAO bound to draw
    int width_ = 0;
    int height_ = 0;
    int last_pass_count_ = 0;
};

}  // namespace eng
