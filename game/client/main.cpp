#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/core/log.h"
#include "engine/core/time.h"
#include "engine/core/version.h"
#include "engine/debug/imgui_layer.h"
#include "engine/platform/input.h"
#include "engine/platform/window.h"
#include "engine/rendering/camera.h"
#include "engine/rendering/debug_draw.h"
#include "engine/rendering/gl_util.h"
#include "engine/rendering/gpu_mesh.h"
#include "engine/rendering/shader.h"
#include "engine/rendering/texture.h"

#include "game/client/fly_camera.h"

namespace {

constexpr double kTickRate = 60.0;

// Blinn-Phong with one directional light. Built-in engine-style shader kept
// inline until the asset system lands in Milestone 3.
constexpr std::string_view kLitVertexSource = R"(#version 410 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform mat4 u_model;
uniform mat3 u_normal_matrix;
uniform mat4 u_view_projection;
out vec3 v_world_pos;
out vec3 v_normal;
out vec2 v_uv;
void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_pos = world.xyz;
    v_normal = u_normal_matrix * a_normal;
    v_uv = a_uv;
    gl_Position = u_view_projection * world;
}
)";

constexpr std::string_view kLitFragmentSource = R"(#version 410 core
in vec3 v_world_pos;
in vec3 v_normal;
in vec2 v_uv;
uniform sampler2D u_base_color;
uniform vec3 u_tint;
uniform vec3 u_light_direction;   // FROM the light, normalized
uniform vec3 u_light_color;
uniform vec3 u_ambient;
uniform vec3 u_camera_pos;
out vec4 o_color;
void main() {
    vec3 albedo = texture(u_base_color, v_uv).rgb * u_tint;
    vec3 n = normalize(v_normal);
    vec3 l = normalize(-u_light_direction);
    float n_dot_l = max(dot(n, l), 0.0);

    vec3 view_dir = normalize(u_camera_pos - v_world_pos);
    vec3 half_dir = normalize(l + view_dir);
    float spec = pow(max(dot(n, half_dir), 0.0), 32.0) * 0.25;

    vec3 color = albedo * (u_ambient + u_light_color * n_dot_l) + u_light_color * spec * n_dot_l;
    o_color = vec4(color, 1.0);
}
)";

struct ClientArgs {
    std::optional<double> run_seconds;
};

ClientArgs parse_args(int argc, char** argv) {
    ClientArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--run-seconds" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            double seconds = 0.0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (ec == std::errc{} && ptr == value.data() + value.size()) {
                args.run_seconds = seconds;
            } else {
                eng::log::warn("Ignoring invalid --run-seconds value '{}'", value);
            }
        } else {
            eng::log::warn("Unknown argument '{}'", arg);
        }
    }
    return args;
}

struct SceneObject {
    glm::vec3 position;
    glm::vec3 scale;
    glm::vec3 tint;
};

std::vector<SceneObject> build_demo_scene() {
    std::vector<SceneObject> objects;
    // Ground slab.
    objects.push_back({{0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {0.85f, 0.85f, 0.9f}});
    // Grid of cubes with varied tints and heights.
    for (int x = -4; x <= 4; ++x) {
        for (int z = -4; z <= 4; ++z) {
            if (x == 0 && z == 0) {
                continue;
            }
            const float height = 1.0f + static_cast<float>((x * x + z * z) % 5) * 0.4f;
            objects.push_back({
                {static_cast<float>(x) * 3.0f, height * 0.5f, static_cast<float>(z) * 3.0f},
                {1.0f, height, 1.0f},
                {0.4f + 0.06f * static_cast<float>(x + 4), 0.5f,
                 0.4f + 0.06f * static_cast<float>(z + 4)},
            });
        }
    }
    return objects;
}

}  // namespace

int main(int argc, char** argv) {
    eng::log::set_level(eng::log::Level::Debug);
    eng::log::info("FPS client starting (engine v{})", eng::version_string());

    const ClientArgs args = parse_args(argc, argv);

    auto window = eng::Window::create({.title = "FPS", .width = 1280, .height = 720});
    if (!window) {
        return 1;
    }
    auto imgui = eng::ImGuiLayer::create(*window);
    if (!imgui) {
        return 1;
    }
    auto lit_shader = eng::Shader::create("lit", kLitVertexSource, kLitFragmentSource);
    auto debug_draw = eng::DebugDraw::create();
    if (!lit_shader || !debug_draw) {
        return 1;
    }

    const eng::GpuMesh cube = eng::GpuMesh::upload(eng::MeshData::unit_cube());
    const eng::Texture2D checker =
        eng::Texture2D::checkerboard(256, 8, {220, 220, 220}, {130, 130, 140});

    const std::vector<SceneObject> scene = build_demo_scene();

    game::FlyCamera fly;
    fly.camera.position = {0.0f, 3.0f, 10.0f};

    eng::InputState input;
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / kTickRate};

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_FRAMEBUFFER_SRGB);

    // Frame time history for the debug graph.
    std::array<float, 240> frame_ms_history{};
    std::size_t frame_ms_cursor = 0;

    bool running = true;
    while (running) {
        if (!window->poll(input, &*imgui)) {
            running = false;
        }
        if (input.was_pressed(eng::Key::Escape)) {
            window->set_relative_mouse(!window->relative_mouse());
        }

        const double dt = clock.tick();
        step.advance(dt);
        while (step.consume_tick()) {
            // Gameplay simulation arrives in Milestone 4+.
        }
        fly.update(input, static_cast<float>(dt), window->relative_mouse());
        fly.camera.aspect = window->aspect();

        // --- render ---------------------------------------------------
        glViewport(0, 0, window->width_px(), window->height_px());
        glClearColor(0.055f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const glm::mat4 view_projection = fly.camera.view_projection();
        int draw_calls = 0;

        lit_shader->bind();
        lit_shader->set_mat4("u_view_projection", view_projection);
        lit_shader->set_vec3("u_light_direction", glm::normalize(glm::vec3{-0.4f, -1.0f, -0.3f}));
        lit_shader->set_vec3("u_light_color", {1.0f, 0.97f, 0.9f});
        lit_shader->set_vec3("u_ambient", {0.18f, 0.20f, 0.24f});
        lit_shader->set_vec3("u_camera_pos", fly.camera.position);
        lit_shader->set_int("u_base_color", 0);
        checker.bind(0);

        for (const SceneObject& object : scene) {
            glm::mat4 model = glm::translate(glm::mat4{1.0f}, object.position);
            model = glm::scale(model, object.scale);
            lit_shader->set_mat4("u_model", model);
            lit_shader->set_mat3("u_normal_matrix",
                                 glm::mat3(glm::transpose(glm::inverse(model))));
            lit_shader->set_vec3("u_tint", object.tint);
            cube.draw();
            ++draw_calls;
        }

        debug_draw->axes(glm::mat4{1.0f}, 2.0f);
        draw_calls += debug_draw->flush(view_projection) > 0 ? 1 : 0;

        // --- debug UI ---------------------------------------------------
        imgui->begin_frame();
        frame_ms_history[frame_ms_cursor] = static_cast<float>(dt * 1000.0);
        frame_ms_cursor = (frame_ms_cursor + 1) % frame_ms_history.size();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Debug");
        ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                    1000.0f / ImGui::GetIO().Framerate);
        ImGui::PlotLines("frame ms", frame_ms_history.data(),
                         static_cast<int>(frame_ms_history.size()),
                         static_cast<int>(frame_ms_cursor), nullptr, 0.0f, 33.3f, {220, 60});
        ImGui::Text("draw calls: %d", draw_calls);
        ImGui::Text("ticks: %llu", static_cast<unsigned long long>(step.tick_count()));
        ImGui::Text("cam: (%.1f, %.1f, %.1f) yaw %.2f pitch %.2f", fly.camera.position.x,
                    fly.camera.position.y, fly.camera.position.z, fly.camera.yaw,
                    fly.camera.pitch);
        ImGui::TextDisabled("Esc: toggle mouse capture");
        ImGui::End();
        imgui->end_frame();

        window->swap();

#if defined(ENG_ENABLE_ASSERTS)
        eng::check_gl_errors("frame end");
#endif

        if (args.run_seconds && clock.elapsed() >= *args.run_seconds) {
            eng::log::info("--run-seconds elapsed; quitting (draw_calls={})", draw_calls);
            running = false;
        }
    }

    eng::log::info("FPS client shutting down cleanly");
    return 0;
}
