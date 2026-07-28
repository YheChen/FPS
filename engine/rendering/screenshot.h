#pragma once

#include <filesystem>
#include <optional>

namespace eng {

// Reads the current framebuffer back to the CPU and writes it as a PNG.
// Intended for verification and bug reports, not for a fast path: the
// glReadPixels stalls the pipeline, so call it at most once per frame and
// never in a shipping hot loop.
//
// `width`/`height` are in pixels (use the drawable size, not the window's
// logical size, on HiDPI displays). Returns false and logs on failure.
bool save_framebuffer_png(const std::filesystem::path& path, int width, int height);

// A few numbers describing what is actually on screen, cheap enough to hand
// to an automated check that has no way to look at a PNG.
//
// The question these answer is "did the renderer draw a scene, or is this a
// flat clear colour?" -- which is what a failed shader compile, a dead
// framebuffer, or an early exit all look like from outside the process.
struct FramebufferSignature {
    // Populated buckets after quantising RGB to 5 bits per channel, so 1 for
    // a single flat colour and up to 32768 for a busy frame. Quantising is
    // what makes this robust: a real frame differs by hundreds of buckets
    // between drivers, but a broken one is always 1 or 2.
    int distinct_colors = 0;
    float mean_luma = 0.0f;  // 0..1, Rec. 709 weights
};

// Same caveat as save_framebuffer_png: one synchronous readback, call it
// sparingly. Returns nullopt and logs on an invalid size.
std::optional<FramebufferSignature> read_framebuffer_signature(int width, int height);

}  // namespace eng
