#include "engine/rendering/postfx.h"

#include "engine/rendering/gl.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "engine/core/log.h"
#include "engine/rendering/postfx_math.h"

namespace eng {

namespace {

// One fullscreen triangle from gl_VertexID -- no vertex buffer, no index
// buffer. A triangle rather than a quad: it covers the screen in one
// primitive with no diagonal seam for the rasteriser to straddle.
constexpr std::string_view kFullscreenVertex = R"(
out vec2 v_uv;
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Bright pass. Keeps only what should bloom, weighted by a soft knee so
// pixels do not pop in and out as they cross the threshold.
// Mirrors eng::bloom_weight / eng::luminance in postfx_math.cpp.
constexpr std::string_view kBrightFragment = R"(
in vec2 v_uv;
uniform sampler2D u_scene;
uniform float u_threshold;
uniform float u_knee;
out vec4 o_color;
void main() {
    vec3 color = texture(u_scene, v_uv).rgb;
    float l = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float weight;
    if (u_knee <= 0.0) {
        weight = l > u_threshold ? 1.0 : 0.0;
    } else {
        float t = clamp((l - (u_threshold - u_knee)) / (2.0 * u_knee), 0.0, 1.0);
        weight = t * t * (3.0 - 2.0 * t);
    }
    o_color = vec4(color * weight, 1.0);
}
)";

// Separable Gaussian, run once horizontally and once vertically. Weights are
// a 9-tap kernel collapsed to 5 samples using linear filtering between
// texels, which is why the offsets are fractional.
constexpr std::string_view kBlurFragment = R"(
in vec2 v_uv;
uniform sampler2D u_source;
uniform vec2 u_direction;   // (1/width, 0) or (0, 1/height)
out vec4 o_color;
void main() {
    const float offsets[3] = float[3](0.0, 1.3846153846, 3.2307692308);
    const float weights[3] = float[3](0.2270270270, 0.3162162162, 0.0702702703);
    vec3 sum = texture(u_source, v_uv).rgb * weights[0];
    for (int i = 1; i < 3; ++i) {
        vec2 delta = u_direction * offsets[i];
        sum += texture(u_source, v_uv + delta).rgb * weights[i];
        sum += texture(u_source, v_uv - delta).rgb * weights[i];
    }
    o_color = vec4(sum, 1.0);
}
)";

// Composite bloom, apply exposure, ACES tonemap.
// Mirrors eng::aces_tonemap in postfx_math.cpp.
constexpr std::string_view kCompositeFragment = R"(
in vec2 v_uv;
uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float u_exposure;
uniform float u_bloom_intensity;
out vec4 o_color;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 color = texture(u_scene, v_uv).rgb;
    color += texture(u_bloom, v_uv).rgb * u_bloom_intensity;
    o_color = vec4(aces(max(color * u_exposure, 0.0)), 1.0);
}
)";

// FXAA 3.11 console-quality variant: cheap, and enough to take the hard edges
// off the arena's long straight lines. Runs on tonemapped LDR values, which
// is where luma-based edge detection belongs.
constexpr std::string_view kFxaaFragment = R"(
in vec2 v_uv;
uniform sampler2D u_source;
uniform vec2 u_texel;
out vec4 o_color;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec3 center = texture(u_source, v_uv).rgb;
    float l_center = luma(center);
    float l_nw = luma(texture(u_source, v_uv + vec2(-u_texel.x, -u_texel.y)).rgb);
    float l_ne = luma(texture(u_source, v_uv + vec2( u_texel.x, -u_texel.y)).rgb);
    float l_sw = luma(texture(u_source, v_uv + vec2(-u_texel.x,  u_texel.y)).rgb);
    float l_se = luma(texture(u_source, v_uv + vec2( u_texel.x,  u_texel.y)).rgb);

    float l_min = min(l_center, min(min(l_nw, l_ne), min(l_sw, l_se)));
    float l_max = max(l_center, max(max(l_nw, l_ne), max(l_sw, l_se)));
    // Flat enough to leave alone. Without this early out, FXAA smears
    // texture detail across the whole frame.
    if (l_max - l_min < 0.06 * l_max + 0.0312) {
        o_color = vec4(center, 1.0);
        return;
    }

    vec2 direction = vec2(-((l_nw + l_ne) - (l_sw + l_se)),
                           ((l_nw + l_sw) - (l_ne + l_se)));
    float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + 1e-5);
    direction = clamp(direction * scale, vec2(-8.0), vec2(8.0)) * u_texel;

    vec3 a = 0.5 * (texture(u_source, v_uv + direction * (1.0 / 3.0 - 0.5)).rgb +
                    texture(u_source, v_uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(u_source, v_uv + direction * -0.5).rgb +
                               texture(u_source, v_uv + direction * 0.5).rgb);
    float l_b = luma(b);
    o_color = vec4((l_b < l_min || l_b > l_max) ? a : b, 1.0);
}
)";

}  // namespace

std::optional<PostFx> PostFx::create(int width, int height) {
    PostFx fx;

    fx.bright_ = Shader::create("postfx_bright", kFullscreenVertex, kBrightFragment);
    fx.blur_ = Shader::create("postfx_blur", kFullscreenVertex, kBlurFragment);
    fx.composite_ = Shader::create("postfx_composite", kFullscreenVertex, kCompositeFragment);
    fx.fxaa_ = Shader::create("postfx_fxaa", kFullscreenVertex, kFxaaFragment);
    if (!fx.bright_ || !fx.blur_ || !fx.composite_ || !fx.fxaa_) {
        return std::nullopt;
    }

    glGenVertexArrays(1, &fx.empty_vao_);

    if (!fx.resize(width, height)) {
        glDeleteVertexArrays(1, &fx.empty_vao_);
        return std::nullopt;
    }

    // Report the size actually allocated, which resize() may have clamped.
    log::info("Post-processing: {}x{}, scene target {}", fx.width(), fx.height(),
              fx.hdr() ? "RGBA16F" : "RGBA8 (no HDR)");
    return fx;
}

bool PostFx::resize(int width, int height) {
    // A degenerate size is clamped rather than rejected. The browser canvas
    // reports 0x0 until it has been laid out, so failing here would abort
    // startup on the web while working fine natively; the frame loop resizes
    // again once the real size arrives.
    width = std::max(1, width);
    height = std::max(1, height);

    const int half_width = half_resolution(width);
    const int half_height = half_resolution(height);

    auto scene = RenderTarget::create(width, height, /*hdr=*/true, /*with_depth=*/true);
    auto bloom_a = RenderTarget::create(half_width, half_height, true, false);
    auto bloom_b = RenderTarget::create(half_width, half_height, true, false);
    auto ldr = RenderTarget::create(width, height, /*hdr=*/false, false);
    if (!scene || !bloom_a || !bloom_b || !ldr) {
        return false;
    }

    scene_ = std::move(scene);
    bloom_a_ = std::move(bloom_a);
    bloom_b_ = std::move(bloom_b);
    ldr_ = std::move(ldr);
    width_ = width;
    height_ = height;
    return true;
}

PostFx::~PostFx() {
    if (empty_vao_ != 0) {
        glDeleteVertexArrays(1, &empty_vao_);
    }
}

PostFx::PostFx(PostFx&& other) noexcept
    : scene_(std::move(other.scene_)),
      bloom_a_(std::move(other.bloom_a_)),
      bloom_b_(std::move(other.bloom_b_)),
      ldr_(std::move(other.ldr_)),
      bright_(std::move(other.bright_)),
      blur_(std::move(other.blur_)),
      composite_(std::move(other.composite_)),
      fxaa_(std::move(other.fxaa_)),
      empty_vao_(std::exchange(other.empty_vao_, 0u)),
      width_(other.width_),
      height_(other.height_),
      last_pass_count_(other.last_pass_count_) {}

PostFx& PostFx::operator=(PostFx&& other) noexcept {
    if (this != &other) {
        if (empty_vao_ != 0) {
            glDeleteVertexArrays(1, &empty_vao_);
        }
        scene_ = std::move(other.scene_);
        bloom_a_ = std::move(other.bloom_a_);
        bloom_b_ = std::move(other.bloom_b_);
        ldr_ = std::move(other.ldr_);
        bright_ = std::move(other.bright_);
        blur_ = std::move(other.blur_);
        composite_ = std::move(other.composite_);
        fxaa_ = std::move(other.fxaa_);
        empty_vao_ = std::exchange(other.empty_vao_, 0u);
        width_ = other.width_;
        height_ = other.height_;
        last_pass_count_ = other.last_pass_count_;
    }
    return *this;
}

bool PostFx::hdr() const {
    return scene_ && scene_->hdr();
}

void PostFx::begin_scene(const glm::vec4& clear_color) const {
    scene_->bind();
    // resolve() leaves these off for its fullscreen passes; the 3D scene
    // needs them back. Forgetting this renders the world with no depth test
    // at all, which looks like random geometry sorting.
    glEnable(GL_DEPTH_TEST);
    glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void PostFx::draw_fullscreen() const {
    glBindVertexArray(empty_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void PostFx::resolve(const Settings& settings) {
    last_pass_count_ = 0;

    // Fullscreen passes never need depth or blending; leaving either on is a
    // classic way to lose the whole post chain to a depth test.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    const bool bloom = settings.bloom && settings.bloom_intensity > 0.0f;

    if (bloom) {
        bloom_a_->bind();
        bright_->bind();
        bright_->set_int("u_scene", 0);
        bright_->set_float("u_threshold", settings.bloom_threshold);
        bright_->set_float("u_knee", settings.bloom_knee);
        scene_->bind_color(0);
        draw_fullscreen();
        ++last_pass_count_;

        const auto width = static_cast<float>(bloom_a_->width());
        const auto height = static_cast<float>(bloom_a_->height());

        blur_->bind();
        blur_->set_int("u_source", 0);

        bloom_b_->bind();
        blur_->set_vec2("u_direction", {1.0f / width, 0.0f});
        bloom_a_->bind_color(0);
        draw_fullscreen();
        ++last_pass_count_;

        bloom_a_->bind();
        blur_->set_vec2("u_direction", {0.0f, 1.0f / height});
        bloom_b_->bind_color(0);
        draw_fullscreen();
        ++last_pass_count_;
    }

    // Composite + tonemap. With bloom off, u_bloom is still bound to
    // something valid (an unsampled texture unit is undefined behaviour on
    // some drivers) but scaled to zero.
    const bool fxaa = settings.fxaa;
    if (fxaa) {
        ldr_->bind();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
    }
    composite_->bind();
    composite_->set_int("u_scene", 0);
    composite_->set_int("u_bloom", 1);
    composite_->set_float("u_exposure", settings.exposure);
    composite_->set_float("u_bloom_intensity", bloom ? settings.bloom_intensity : 0.0f);
    scene_->bind_color(0);
    bloom_a_->bind_color(1);
    draw_fullscreen();
    ++last_pass_count_;

    if (fxaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        fxaa_->bind();
        fxaa_->set_int("u_source", 0);
        fxaa_->set_vec2("u_texel",
                        {1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_)});
        ldr_->bind_color(0);
        draw_fullscreen();
        ++last_pass_count_;
    }

    // The HUD and ImGui draw after this and expect a normal 2D state.
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace eng
