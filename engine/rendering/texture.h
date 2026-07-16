#pragma once

#include <cstdint>
#include <span>

#include <glm/glm.hpp>

namespace eng {

// RAII 2D texture, move-only. RGBA8 only for now; mipmapped, repeat wrap.
class Texture2D {
public:
    // `pixels` is tightly packed RGBA8, row-major, bottom-left origin
    // (OpenGL convention), size = width * height * 4 bytes.
    // `srgb` selects GL_SRGB8_ALPHA8 storage (use for albedo/color data).
    static Texture2D from_pixels(int width, int height, std::span<const std::uint8_t> pixels,
                                 bool srgb);

    // Procedural checkerboard, `cells` squares per side. Useful as a test
    // pattern and as the "missing texture" fallback.
    static Texture2D checkerboard(int size_px, int cells, glm::u8vec3 color_a, glm::u8vec3 color_b);

    ~Texture2D();
    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    void bind(std::uint32_t unit) const;

    int width() const { return width_; }
    int height() const { return height_; }

private:
    Texture2D() = default;

    std::uint32_t id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace eng
