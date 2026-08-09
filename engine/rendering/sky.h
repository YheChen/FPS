#pragma once

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "engine/rendering/shader.h"

namespace eng {

// Procedural sky for the background of the 3D pass. Move-only, RAII.
//
// No cubemap and no texture: a horizon-to-zenith gradient and a sun disc are
// cheaper to evaluate than they are to fetch, and they cost nothing in
// download size -- which matters when every asset byte is baked into the
// browser build's MEMFS.
//
// The point of the sun disc is agreement. The scene is lit by one directional
// light and everything in it casts a shadow away from that light; with a flat
// clear colour behind it, nothing in the frame explains where those shadows
// come from. draw() takes the same direction vector the lit shader gets, so
// the two cannot drift apart.
//
// Draw it INTO the HDR post-processing target, after the opaque scene -- see
// draw() for why after.
class Sky {
public:
    struct Params {
        // A clear late-afternoon sky: a hazy pale band at the horizon fading
        // to blue overhead. Both below 1.0 so the bright pass ignores them --
        // only the sun is meant to bloom, and a whole hemisphere over the
        // bloom threshold would veil the entire frame.
        glm::vec3 horizon_color{0.38f, 0.44f, 0.54f};
        glm::vec3 zenith_color{0.09f, 0.18f, 0.40f};
        // Below the horizon. Never visible from inside a sealed arena, but
        // the free camera can leave one, and a black lower hemisphere reads
        // as a broken frame rather than as ground.
        glm::vec3 ground_color{0.13f, 0.13f, 0.14f};
        // Exponent on the elevation. Below 1.0 it compresses the pale band
        // into the first few degrees above the horizon, which is what makes
        // it read as sky instead of as a linear ramp; at 0.5 the gradient is
        // half way to the zenith colour about 14 degrees up.
        float horizon_falloff = 0.5f;

        glm::vec3 sun_color{1.0f, 0.94f, 0.82f};
        // Radiance of the disc. Far above 1.0 on purpose: the tonemap needs
        // headroom to roll off, and the bloom threshold is 1.0, so this is
        // what puts glare around the sun.
        float sun_intensity = 14.0f;
        // The real sun subtends 0.0093 rad, which at a 70 degree vertical
        // field of view is about five pixels -- too small to read as
        // anything, and small enough to crawl under the half-resolution
        // bright pass. Two degrees is a deliberate lie that looks right.
        float sun_angular_radius = 0.035f;
        // Glow around the disc, as a fraction of sun_intensity. This is the
        // part bloom actually picks up: a hard-edged disc a couple of degrees
        // across aliases in the half-res bright pass, a soft halo does not.
        // 0.10 * 14 puts the glow just over the 1.0 bloom threshold where it
        // meets the disc, and under it a fraction of a degree further out --
        // so the disc keeps a visible edge instead of dissolving into glare.
        float halo_intensity = 0.10f;
        // Larger = tighter glow. exp(-(1 - cos angle) * falloff), so the glow
        // reaches 1/e at roughly sqrt(2 / falloff) radians -- about 5 degrees
        // here, and gone by 12. The first version of this was 70, which held
        // the sky saturated for 20-odd degrees around the sun and read as a
        // white hole rather than as a sun.
        float halo_falloff = 220.0f;
    };

    static std::optional<Sky> create();

    ~Sky();
    Sky(Sky&& other) noexcept;
    Sky& operator=(Sky&& other) noexcept;
    Sky(const Sky&) = delete;
    Sky& operator=(const Sky&) = delete;

    // One fullscreen triangle, no vertex buffer. `sun_direction` is the
    // direction the light TRAVELS (the same vector the lit shader gets as
    // u_light_direction), so the disc lands where the shadows say it should.
    //
    // Call this AFTER the opaque scene, with the depth buffer still holding
    // it and the depth test enabled. Depth func and depth writes are left
    // back at the GL defaults the scene pass runs with (GL_LESS, writes on),
    // the same way ParticleRenderer::draw restores them.
    void draw(const glm::mat4& view_projection, const glm::vec3& camera_position,
              const glm::vec3& sun_direction, const Params& params) const;

private:
    Sky() = default;

    std::optional<Shader> shader_;
    std::uint32_t empty_vao_ = 0;  // core profile needs a VAO bound to draw
};

}  // namespace eng
