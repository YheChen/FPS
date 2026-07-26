#pragma once

#include <filesystem>

namespace eng {

// Reads the current framebuffer back to the CPU and writes it as a PNG.
// Intended for verification and bug reports, not for a fast path: the
// glReadPixels stalls the pipeline, so call it at most once per frame and
// never in a shipping hot loop.
//
// `width`/`height` are in pixels (use the drawable size, not the window's
// logical size, on HiDPI displays). Returns false and logs on failure.
bool save_framebuffer_png(const std::filesystem::path& path, int width, int height);

}  // namespace eng
