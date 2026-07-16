#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"
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
#include "engine/scene/scene.h"

#include "game/client/fly_camera.h"

namespace {

constexpr double kTickRate = 60.0;

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

// GPU-side copy of one glTF mesh: one GpuMesh per primitive plus its
// material color.
struct RenderPrimitive {
    eng::GpuMesh gpu;
    glm::vec3 color{1.0f};
};

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
    auto lit_shader = eng::Shader::create("lit", kLitVertexSource, kLitFragmentSource);
    auto debug_draw = eng::DebugDraw::create();
    if (!imgui || !lit_shader || !debug_draw) {
        return 1;
    }

    // --- assets ----------------------------------------------------------
    const auto assets_root = eng::find_assets_root();
    if (!assets_root) {
        eng::log::error("Could not locate the assets/ directory; run from the repo or build tree");
        return 1;
    }
    eng::AssetCache assets{*assets_root};
    const eng::GltfModel* arena = assets.model("maps/arena01.glb");
    if (arena == nullptr) {
        return 1;
    }

    // Upload every glTF mesh primitive once; entities reference mesh indices.
    std::vector<std::vector<RenderPrimitive>> render_meshes;
    render_meshes.reserve(arena->meshes.size());
    for (const eng::GltfMesh& mesh : arena->meshes) {
        std::vector<RenderPrimitive> primitives;
        for (const eng::GltfPrimitive& primitive : mesh.primitives) {
            RenderPrimitive rp{eng::GpuMesh::upload(primitive.mesh), glm::vec3{1.0f}};
            if (primitive.material >= 0) {
                rp.color = glm::vec3(
                    arena->materials[static_cast<std::size_t>(primitive.material)].base_color);
            }
            primitives.push_back(std::move(rp));
        }
        render_meshes.push_back(std::move(primitives));
    }

    // --- scene -------------------------------------------------------------
    eng::Scene scene;
    int spawn_markers = 0;
    for (const eng::GltfNode& node : arena->nodes) {
        const eng::EntityId id = scene.create(node.name);
        eng::Entity* entity = scene.get(id);
        entity->transform = eng::Transform::from_matrix(node.transform);
        entity->mesh = node.mesh;
        if (node.mesh < 0) {
            ++spawn_markers;
        }
    }
    eng::log::info("Scene: {} entities ({} markers)", scene.count(), spawn_markers);

    const eng::Texture2D white = eng::Texture2D::checkerboard(4, 1, {255, 255, 255}, {255, 255, 255});

    game::FlyCamera fly;
    fly.camera.position = {0.0f, 6.0f, 18.0f};
    fly.camera.pitch = -0.25f;

    eng::InputState input;
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / kTickRate};

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_FRAMEBUFFER_SRGB);

    std::array<float, 240> frame_ms_history{};
    std::size_t frame_ms_cursor = 0;
    eng::EntityId selected{};
    bool show_markers = true;

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

        // --- render ----------------------------------------------------
        glViewport(0, 0, window->width_px(), window->height_px());
        glClearColor(0.055f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const glm::mat4 view_projection = fly.camera.view_projection();
        int draw_calls = 0;

        lit_shader->bind();
        lit_shader->set_mat4("u_view_projection", view_projection);
        lit_shader->set_vec3("u_light_direction", glm::normalize(glm::vec3{-0.4f, -1.0f, -0.3f}));
        lit_shader->set_vec3("u_light_color", {1.0f, 0.97f, 0.9f});
        lit_shader->set_vec3("u_ambient", {0.20f, 0.22f, 0.26f});
        lit_shader->set_vec3("u_camera_pos", fly.camera.position);
        lit_shader->set_int("u_base_color", 0);
        white.bind(0);

        scene.each([&](eng::EntityId, eng::Entity& entity) {
            if (!entity.visible || entity.mesh < 0) {
                return;
            }
            const glm::mat4 model = entity.transform.to_matrix();
            lit_shader->set_mat4("u_model", model);
            lit_shader->set_mat3("u_normal_matrix", glm::mat3(glm::transpose(glm::inverse(model))));
            for (const RenderPrimitive& primitive :
                 render_meshes[static_cast<std::size_t>(entity.mesh)]) {
                lit_shader->set_vec3("u_tint", primitive.color * glm::vec3(entity.tint));
                primitive.gpu.draw();
                ++draw_calls;
            }
        });

        debug_draw->axes(glm::mat4{1.0f}, 2.0f);
        if (show_markers) {
            scene.each([&](eng::EntityId, eng::Entity& entity) {
                if (entity.mesh < 0) {
                    const glm::vec3 p = entity.transform.position;
                    debug_draw->aabb(p - glm::vec3{0.3f, 0.0f, 0.3f}, p + glm::vec3{0.3f, 1.8f, 0.3f},
                                     {1.0f, 0.8f, 0.1f});
                }
            });
        }
        draw_calls += debug_draw->flush(view_projection) > 0 ? 1 : 0;

        // --- debug UI ----------------------------------------------------
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
        ImGui::Text("cam: (%.1f, %.1f, %.1f)", fly.camera.position.x, fly.camera.position.y,
                    fly.camera.position.z);
        ImGui::Checkbox("show spawn markers", &show_markers);
        ImGui::TextDisabled("Esc: toggle mouse capture");
        ImGui::End();

        ImGui::SetNextWindowPos({10, 240}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({320, 380}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Entities");
        scene.each([&](eng::EntityId id, eng::Entity& entity) {
            const bool is_selected = id == selected;
            if (ImGui::Selectable(entity.name.c_str(), is_selected)) {
                selected = id;
            }
        });
        ImGui::Separator();
        if (eng::Entity* entity = scene.get(selected)) {
            ImGui::Text("%s", entity->name.c_str());
            ImGui::DragFloat3("position", &entity->transform.position.x, 0.1f);
            ImGui::DragFloat3("scale", &entity->transform.scale.x, 0.1f, 0.01f, 100.0f);
            ImGui::Checkbox("visible", &entity->visible);
        } else {
            ImGui::TextDisabled("select an entity");
        }
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
