#pragma once

#include <optional>

union SDL_Event;

namespace eng {

class Window;

// Dear ImGui integration (SDL3 + OpenGL 3/4 backends). One instance per
// window; owns the ImGui context. Non-movable because the ImGui context is
// process-global.
class ImGuiLayer {
public:
    static std::optional<ImGuiLayer> create(Window& window);

    ~ImGuiLayer();
    ImGuiLayer(ImGuiLayer&& other) noexcept;
    ImGuiLayer& operator=(ImGuiLayer&&) = delete;
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Called by Window::poll for every SDL event.
    void process_event(const SDL_Event& event);

    void begin_frame();
    void end_frame();  // renders the draw data

    // True when ImGui wants the mouse/keyboard (game input should ignore it).
    bool wants_mouse() const;
    bool wants_keyboard() const;

private:
    ImGuiLayer() = default;
    bool active_ = false;
};

}  // namespace eng
