#include "engine/platform/window.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <utility>

#include "engine/core/log.h"

namespace eng {

namespace {

// Maps the SDL scancodes we care about onto engine keys.
std::optional<Key> translate_scancode(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_W:
            return Key::W;
        case SDL_SCANCODE_A:
            return Key::A;
        case SDL_SCANCODE_S:
            return Key::S;
        case SDL_SCANCODE_D:
            return Key::D;
        case SDL_SCANCODE_R:
            return Key::R;
        case SDL_SCANCODE_SPACE:
            return Key::Space;
        case SDL_SCANCODE_LSHIFT:
            return Key::LeftShift;
        case SDL_SCANCODE_TAB:
            return Key::Tab;
        case SDL_SCANCODE_ESCAPE:
            return Key::Escape;
        case SDL_SCANCODE_RETURN:
            return Key::Enter;
        case SDL_SCANCODE_F1:
            return Key::F1;
        case SDL_SCANCODE_F2:
            return Key::F2;
        case SDL_SCANCODE_F3:
            return Key::F3;
        default:
            return std::nullopt;
    }
}

std::optional<MouseButton> translate_mouse_button(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT:
            return MouseButton::Left;
        case SDL_BUTTON_RIGHT:
            return MouseButton::Right;
        default:
            return std::nullopt;
    }
}

}  // namespace

std::optional<Window> Window::create(const WindowConfig& config) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        log::error("SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
        return std::nullopt;
    }

    // OpenGL 4.1 core: the highest version available on macOS (ADR 0003).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    Window window;
    window.window_ =
        SDL_CreateWindow(config.title.c_str(), config.width, config.height,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window.window_ == nullptr) {
        log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return std::nullopt;
    }

    window.gl_context_ = SDL_GL_CreateContext(window.window_);
    if (window.gl_context_ == nullptr) {
        log::error("SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(window.window_);
        window.window_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return std::nullopt;
    }

    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)) == 0) {
        log::error("gladLoadGL failed: could not load OpenGL functions");
        return std::nullopt;  // destructor cleans up window/context
    }

    if (!SDL_GL_SetSwapInterval(config.vsync ? 1 : 0)) {
        log::warn("SDL_GL_SetSwapInterval failed: {}", SDL_GetError());
    }

    SDL_GetWindowSizeInPixels(window.window_, &window.width_px_, &window.height_px_);

    log::info("Window created: {}x{} px, OpenGL {} ({})", window.width_px_, window.height_px_,
              reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    return window;
}

Window::~Window() {
    if (gl_context_ != nullptr) {
        SDL_GL_DestroyContext(static_cast<SDL_GLContext>(gl_context_));
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

Window::Window(Window&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)),
      gl_context_(std::exchange(other.gl_context_, nullptr)),
      width_px_(other.width_px_),
      height_px_(other.height_px_) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        this->~Window();
        window_ = std::exchange(other.window_, nullptr);
        gl_context_ = std::exchange(other.gl_context_, nullptr);
        width_px_ = other.width_px_;
        height_px_ = other.height_px_;
    }
    return *this;
}

bool Window::poll(InputState& input) {
    input.begin_frame();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return false;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!event.key.repeat) {
                    if (const auto key = translate_scancode(event.key.scancode)) {
                        input.apply_key(*key, event.key.down);
                    }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                input.apply_mouse_delta(event.motion.xrel, event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (const auto button = translate_mouse_button(event.button.button)) {
                    input.apply_mouse_button(*button, event.button.down);
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                SDL_GetWindowSizeInPixels(window_, &width_px_, &height_px_);
                break;
            default:
                break;
        }
    }
    return true;
}

void Window::swap() {
    SDL_GL_SwapWindow(window_);
}

float Window::aspect() const {
    return height_px_ > 0 ? static_cast<float>(width_px_) / static_cast<float>(height_px_) : 1.0f;
}

void Window::set_relative_mouse(bool enabled) {
    if (!SDL_SetWindowRelativeMouseMode(window_, enabled)) {
        log::warn("SDL_SetWindowRelativeMouseMode failed: {}", SDL_GetError());
    }
}

bool Window::relative_mouse() const {
    return SDL_GetWindowRelativeMouseMode(window_);
}

}  // namespace eng
