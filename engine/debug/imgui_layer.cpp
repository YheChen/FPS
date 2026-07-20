#include "engine/debug/imgui_layer.h"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>

#include "engine/core/log.h"
#include "engine/platform/window.h"

namespace eng {

std::optional<ImGuiLayer> ImGuiLayer::create(Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini litter

    if (!ImGui_ImplSDL3_InitForOpenGL(window.sdl_window(), window.gl_context())) {
        log::error("ImGui SDL3 backend init failed");
        ImGui::DestroyContext();
        return std::nullopt;
    }
    // The ImGui GL backend compiles its own shaders with this exact version
    // string, so it must match the context: GLSL ES 300 on WebGL2, GLSL 410
    // core on desktop. A desktop string on WebGL2 fails to compile and ImGui
    // silently renders nothing.
#if defined(__EMSCRIPTEN__)
    const char* glsl_version = "#version 300 es";
#else
    const char* glsl_version = "#version 410 core";
#endif
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        log::error("ImGui OpenGL3 backend init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return std::nullopt;
    }

    ImGuiLayer layer;
    layer.active_ = true;
    return layer;
}

ImGuiLayer::~ImGuiLayer() {
    if (active_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
}

ImGuiLayer::ImGuiLayer(ImGuiLayer&& other) noexcept : active_(other.active_) {
    other.active_ = false;
}

void ImGuiLayer::process_event(const SDL_Event& event) {
    if (active_) {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
}

void ImGuiLayer::begin_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::end_frame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool ImGuiLayer::wants_mouse() const {
    return active_ && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wants_keyboard() const {
    return active_ && ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace eng
