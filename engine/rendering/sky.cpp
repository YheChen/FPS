#include "engine/rendering/sky.h"

#include "engine/rendering/gl.h"

#include <cmath>
#include <string_view>
#include <utility>

namespace eng {

namespace {

// The same fullscreen triangle as the post chain, with one difference that is
// the whole trick: z is set equal to w, so after the perspective divide every
// vertex lands exactly on the far plane at depth 1.0. z == w is inside the
// clip volume, not outside it, so nothing is clipped away.
constexpr std::string_view kSkyVertex = R"(
out vec2 v_ndc;
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_ndc = corner * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
)";

// No samplers, so nothing here needs an addition to glsl_preamble().
constexpr std::string_view kSkyFragment = R"(
in vec2 v_ndc;
uniform mat4 u_inv_view_projection;
uniform vec3 u_camera_pos;
uniform vec3 u_to_sun;          // normalized, points AT the sun
uniform vec3 u_horizon_color;
uniform vec3 u_zenith_color;
uniform vec3 u_ground_color;
uniform vec3 u_sun_color;       // already scaled to HDR radiance
uniform float u_horizon_falloff;
uniform float u_sun_cos_outer;  // cos of the disc's angular radius
uniform float u_sun_cos_inner;  // cos of where the soft edge starts
uniform float u_halo_intensity;
uniform float u_halo_falloff;
out vec4 o_color;

void main() {
    // Un-project this pixel's far-plane point back to world space. The
    // inverse projection does not preserve w, so the divide is not optional.
    vec4 far = u_inv_view_projection * vec4(v_ndc, 1.0, 1.0);
    vec3 ray = normalize(far.xyz / far.w - u_camera_pos);

    vec3 color = mix(u_horizon_color, u_zenith_color,
                     pow(clamp(ray.y, 0.0, 1.0), u_horizon_falloff));
    // Fade to ground over the first ~7 degrees below the horizon rather than
    // switching at exactly y = 0, which would draw a hard line across the
    // frame the moment the camera looks down.
    color = mix(color, u_ground_color, clamp(-ray.y * 8.0, 0.0, 1.0));

    float cos_angle = dot(ray, u_to_sun);
    // smoothstep, not step: the disc is a few dozen pixels across and a hard
    // edge on it crawls badly as the camera turns.
    float disc = smoothstep(u_sun_cos_outer, u_sun_cos_inner, cos_angle);
    // 1 - cos(angle) is angle^2/2 for the small angles that matter here, so
    // this is a Gaussian glow without an acos or a huge pow exponent.
    float halo = exp(-(1.0 - cos_angle) * u_halo_falloff) * u_halo_intensity;
    color += u_sun_color * (disc + halo);

    o_color = vec4(color, 1.0);
}
)";

}  // namespace

std::optional<Sky> Sky::create() {
    Sky sky;
    sky.shader_ = Shader::create("sky", kSkyVertex, kSkyFragment);
    if (!sky.shader_) {
        return std::nullopt;
    }
    glGenVertexArrays(1, &sky.empty_vao_);
    return sky;
}

Sky::~Sky() {
    if (empty_vao_ != 0) {
        glDeleteVertexArrays(1, &empty_vao_);
    }
}

Sky::Sky(Sky&& other) noexcept
    : shader_(std::move(other.shader_)), empty_vao_(std::exchange(other.empty_vao_, 0u)) {}

Sky& Sky::operator=(Sky&& other) noexcept {
    if (this != &other) {
        if (empty_vao_ != 0) {
            glDeleteVertexArrays(1, &empty_vao_);
        }
        shader_ = std::move(other.shader_);
        empty_vao_ = std::exchange(other.empty_vao_, 0u);
    }
    return *this;
}

void Sky::draw(const glm::mat4& view_projection, const glm::vec3& camera_position,
               const glm::vec3& sun_direction, const Params& params) const {
    shader_->bind();
    shader_->set_mat4("u_inv_view_projection", glm::inverse(view_projection));
    shader_->set_vec3("u_camera_pos", camera_position);
    shader_->set_vec3("u_to_sun", glm::normalize(-sun_direction));
    shader_->set_vec3("u_horizon_color", params.horizon_color);
    shader_->set_vec3("u_zenith_color", params.zenith_color);
    shader_->set_vec3("u_ground_color", params.ground_color);
    shader_->set_vec3("u_sun_color", params.sun_color * params.sun_intensity);
    shader_->set_float("u_horizon_falloff", params.horizon_falloff);
    shader_->set_float("u_sun_cos_outer", std::cos(params.sun_angular_radius));
    shader_->set_float("u_sun_cos_inner", std::cos(params.sun_angular_radius * 0.55f));
    shader_->set_float("u_halo_intensity", params.halo_intensity);
    shader_->set_float("u_halo_falloff", params.halo_falloff);

    // LEQUAL because the triangle sits exactly on the cleared depth value and
    // the default GL_LESS would reject every fragment of it. Depth writes off
    // because the sky is infinitely far away and has no business occluding
    // the transparent work that follows (particles, tracers) -- and at depth
    // 1.0 the write would be a no-op against the clear anyway.
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glBindVertexArray(empty_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

}  // namespace eng
