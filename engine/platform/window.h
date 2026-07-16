#pragma once

#include <optional>
#include <string>

#include "engine/platform/input.h"

struct SDL_Window;

namespace eng {

struct WindowConfig {
    std::string title = "fps";
    int width = 1280;
    int height = 720;
    bool vsync = true;
};

// Owns the OS window and the OpenGL 4.1 core context (RAII). Move-only.
// Also owns event polling: poll() translates SDL events into an InputState.
//
// Threading: all Window methods must be called from the main thread.
class Window {
public:
    // Returns nullopt (with an error log) if window/context creation or
    // GL function loading fails.
    static std::optional<Window> create(const WindowConfig& config);

    ~Window();
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Pumps all pending OS events into `input` (after input.begin_frame()).
    // Returns false when the user requested quit (window close / OS quit).
    bool poll(InputState& input);

    void swap();

    // Framebuffer size in pixels (handles high-DPI).
    int width_px() const { return width_px_; }
    int height_px() const { return height_px_; }
    float aspect() const;

    // Relative mouse mode: cursor hidden and locked, deltas keep flowing.
    void set_relative_mouse(bool enabled);
    bool relative_mouse() const;

private:
    Window() = default;

    SDL_Window* window_ = nullptr;
    void* gl_context_ = nullptr;  // SDL_GLContext
    int width_px_ = 0;
    int height_px_ = 0;
};

}  // namespace eng
