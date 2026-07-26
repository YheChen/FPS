#pragma once

#include <cstdint>
#include <optional>

namespace eng {

// Depth-only render target for a single directional-light shadow map.
// Move-only, RAII.
//
// One map, no cascades: the arena is ~40 m across, so a single tightly
// fitted 2048² map gives ~2 cm per texel. Cascades only earn their
// complexity on maps large enough that one projection cannot cover them.
//
// The depth texture is created with comparison sampling enabled, so shaders
// sample it as a `sampler2DShadow` and get filtered in/out results directly
// from the hardware rather than comparing depths by hand.
class ShadowMap {
public:
    // `resolution` is the side length in texels. Returns nullopt (logged)
    // if the framebuffer does not come out complete.
    static std::optional<ShadowMap> create(int resolution);

    ~ShadowMap();
    ShadowMap(ShadowMap&& other) noexcept;
    ShadowMap& operator=(ShadowMap&& other) noexcept;
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // Binds the framebuffer, sets the viewport to the map, and clears
    // depth. Draw the shadow casters between begin() and end().
    void begin_depth_pass() const;

    // Restores the default framebuffer and the given viewport.
    void end_depth_pass(int viewport_width, int viewport_height) const;

    void bind_depth(std::uint32_t unit) const;

    int resolution() const { return resolution_; }

private:
    ShadowMap() = default;

    std::uint32_t framebuffer_ = 0;
    std::uint32_t depth_texture_ = 0;
    int resolution_ = 0;
};

}  // namespace eng
