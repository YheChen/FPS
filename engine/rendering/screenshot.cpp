#include "engine/rendering/screenshot.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

#include "engine/core/log.h"
#include "engine/rendering/gl.h"

namespace eng {

namespace {

// stb writes through a callback so the bytes can go out via std::ofstream
// (stb's own stdio path does not handle non-ASCII paths on Windows).
void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

bool save_framebuffer_png(const std::filesystem::path& path, int width, int height) {
    if (width <= 0 || height <= 0) {
        log::error("Screenshot: invalid size {}x{}", width, height);
        return false;
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    std::vector<unsigned char> pixels(w * h * 4);

    // Default pack alignment is 4; RGBA rows are already 4-byte aligned, but
    // be explicit so this keeps working if the format ever changes.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // GL returns bottom-up rows; PNG is top-down.
    std::vector<unsigned char> flipped(pixels.size());
    for (std::size_t y = 0; y < h; ++y) {
        const auto* src = pixels.data() + (h - 1 - y) * w * 4;
        std::copy(src, src + w * 4, flipped.data() + y * w * 4);
    }

    std::vector<unsigned char> encoded;
    if (stbi_write_png_to_func(&append_bytes, &encoded, width, height, 4, flipped.data(),
                               static_cast<int>(w * 4)) == 0) {
        log::error("Screenshot: PNG encoding failed");
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        log::error("Screenshot: cannot open '{}' for writing", path.string());
        return false;
    }
    stream.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!stream) {
        log::error("Screenshot: write to '{}' failed", path.string());
        return false;
    }

    log::info("Screenshot: wrote {} ({}x{}, {} bytes)", path.string(), width, height,
              encoded.size());
    return true;
}

std::optional<FramebufferSignature> read_framebuffer_signature(int width, int height) {
    if (width <= 0 || height <= 0) {
        log::error("Framebuffer signature: invalid size {}x{}", width, height);
        return std::nullopt;
    }

    const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<unsigned char> pixels(pixel_count * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // 32 levels per channel. A bitset rather than a hash set: one 4 KiB
    // allocation and a branch-free insert, against a rehash every few
    // thousand pixels.
    constexpr std::size_t kBuckets = 32u * 32u * 32u;
    std::vector<bool> seen(kBuckets, false);
    std::uint64_t luma_sum = 0;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const unsigned r = pixels[i * 4 + 0];
        const unsigned g = pixels[i * 4 + 1];
        const unsigned b = pixels[i * 4 + 2];
        seen[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)] = true;
        // Integer Rec. 709 (x1000) so the accumulation stays exact.
        luma_sum += 213u * r + 715u * g + 72u * b;
    }

    FramebufferSignature signature;
    signature.distinct_colors = static_cast<int>(std::count(seen.begin(), seen.end(), true));
    signature.mean_luma = static_cast<float>(static_cast<double>(luma_sum) /
                                             (static_cast<double>(pixel_count) * 1000.0 * 255.0));
    return signature;
}

}  // namespace eng
