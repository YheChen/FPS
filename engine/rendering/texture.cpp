#include "engine/rendering/texture.h"

#include <glad/gl.h>

#include <utility>
#include <vector>

#include "engine/core/assert.h"

namespace eng {

Texture2D Texture2D::from_pixels(int width, int height, std::span<const std::uint8_t> pixels,
                                 bool srgb) {
    ENG_ASSERT(width > 0 && height > 0, "texture dimensions must be positive");
    ENG_ASSERT(pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
               "pixel buffer size must be width*height*4 (RGBA8)");

    Texture2D texture;
    texture.width_ = width;
    texture.height_ = height;
    glGenTextures(1, &texture.id_);
    glBindTexture(GL_TEXTURE_2D, texture.id_);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

Texture2D Texture2D::checkerboard(int size_px, int cells, glm::u8vec3 color_a,
                                  glm::u8vec3 color_b) {
    ENG_ASSERT(size_px > 0 && cells > 0, "invalid checkerboard parameters");
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(size_px) * static_cast<std::size_t>(size_px) * 4);
    const int cell_size = size_px / cells;
    for (int y = 0; y < size_px; ++y) {
        for (int x = 0; x < size_px; ++x) {
            const bool a = ((x / cell_size) + (y / cell_size)) % 2 == 0;
            const glm::u8vec3 color = a ? color_a : color_b;
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(size_px) +
                 static_cast<std::size_t>(x)) *
                4;
            pixels[i + 0] = color.r;
            pixels[i + 1] = color.g;
            pixels[i + 2] = color.b;
            pixels[i + 3] = 255;
        }
    }
    return from_pixels(size_px, size_px, pixels, /*srgb=*/true);
}

Texture2D::~Texture2D() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
    }
}

Texture2D::Texture2D(Texture2D&& other) noexcept
    : id_(std::exchange(other.id_, 0u)), width_(other.width_), height_(other.height_) {}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        if (id_ != 0) {
            glDeleteTextures(1, &id_);
        }
        id_ = std::exchange(other.id_, 0u);
        width_ = other.width_;
        height_ = other.height_;
    }
    return *this;
}

void Texture2D::bind(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

}  // namespace eng
