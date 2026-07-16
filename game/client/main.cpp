#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"
#include "engine/audio/audio_engine.h"
#include "engine/core/log.h"
#include "engine/core/time.h"
#include "engine/core/version.h"
#include "engine/debug/imgui_layer.h"
#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
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
#include "game/shared/health.h"
#include "game/shared/hitscan.h"
#include "game/shared/input_command.h"
#include "game/shared/player_movement.h"
#include "game/shared/weapon.h"

namespace {

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
uniform vec3 u_light_direction;
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

struct RenderPrimitive {
    eng::GpuMesh gpu;
    glm::vec3 color{1.0f};
};

void draw_capsule(eng::DebugDraw& draw, const glm::vec3& feet, float radius, float height,
                  const glm::vec3& color) {
    constexpr int kSegments = 16;
    const glm::vec3 bottom = feet + glm::vec3{0.0f, radius, 0.0f};
    const glm::vec3 top = feet + glm::vec3{0.0f, height - radius, 0.0f};
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = static_cast<float>(i) / kSegments * 2.0f * std::numbers::pi_v<float>;
        const float a1 = static_cast<float>(i + 1) / kSegments * 2.0f * std::numbers::pi_v<float>;
        const glm::vec3 r0{std::cos(a0) * radius, 0.0f, std::sin(a0) * radius};
        const glm::vec3 r1{std::cos(a1) * radius, 0.0f, std::sin(a1) * radius};
        draw.line(bottom + r0, bottom + r1, color);
        draw.line(top + r0, top + r1, color);
        if (i % 4 == 0) {
            draw.line(bottom + r0, top + r0, color);
        }
    }
    draw.line(feet, feet + glm::vec3{0.0f, height, 0.0f}, color);
}

// Shootable practice dummy. Rendered as a stretched cube, hit-tested as a
// capsule (same math the networked players will use in Milestone 8).
struct Target {
    glm::vec3 home{0.0f};
    glm::vec3 position{0.0f};
    game::Health health;
    float respawn_remaining = 0.0f;  // > 0 while dead
    float patrol_radius = 0.0f;      // 0 = static
    float patrol_phase = 0.0f;

    bool alive() const { return respawn_remaining <= 0.0f; }
};

struct Tracer {
    glm::vec3 from;
    glm::vec3 to;
    float ttl;
};

game::InputCommand make_command(const eng::InputState& input, float yaw, float pitch,
                                std::uint32_t sequence) {
    game::InputCommand command;
    command.sequence = sequence;
    command.yaw = yaw;
    command.pitch = pitch;
    game::set_button(command, game::Button::Forward, input.is_down(eng::Key::W));
    game::set_button(command, game::Button::Back, input.is_down(eng::Key::S));
    game::set_button(command, game::Button::Left, input.is_down(eng::Key::A));
    game::set_button(command, game::Button::Right, input.is_down(eng::Key::D));
    game::set_button(command, game::Button::Jump, input.is_down(eng::Key::Space));
    game::set_button(command, game::Button::Fire, input.is_down(eng::MouseButton::Left));
    game::set_button(command, game::Button::Reload, input.is_down(eng::Key::R));
    return command;
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
    auto lit_shader = eng::Shader::create("lit", kLitVertexSource, kLitFragmentSource);
    auto debug_draw = eng::DebugDraw::create();
    if (!imgui || !lit_shader || !debug_draw) {
        return 1;
    }

    // --- assets & scene ---------------------------------------------------
    const auto assets_root = eng::find_assets_root();
    if (!assets_root) {
        eng::log::error("Could not locate the assets/ directory");
        return 1;
    }
    eng::AssetCache assets{*assets_root};
    const eng::GltfModel* arena = assets.model("maps/arena01.glb");
    if (arena == nullptr) {
        return 1;
    }

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

    const eng::Texture2D white =
        eng::Texture2D::checkerboard(4, 1, {255, 255, 255}, {255, 255, 255});
    const eng::GpuMesh cube = eng::GpuMesh::upload(eng::MeshData::unit_cube());

    eng::Scene scene;
    std::vector<glm::vec3> spawn_points;
    for (const eng::GltfNode& node : arena->nodes) {
        const eng::EntityId id = scene.create(node.name);
        eng::Entity* entity = scene.get(id);
        entity->transform = eng::Transform::from_matrix(node.transform);
        entity->mesh = node.mesh;
        if (node.name.starts_with("spawn_")) {
            spawn_points.push_back(entity->transform.position);
        }
    }

    // --- physics ------------------------------------------------------------
    eng::PhysicsWorld world;
    for (const eng::GltfNode& node : arena->nodes) {
        if (node.mesh < 0) {
            continue;
        }
        for (const eng::GltfPrimitive& primitive :
             arena->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
            world.add_static_mesh(primitive.mesh, node.transform);
        }
    }
    world.optimize();
    eng::log::info("Physics: {} static bodies", world.body_count());

    const glm::vec3 spawn = spawn_points.empty() ? glm::vec3{0.0f, 1.0f, 0.0f} : spawn_points[0];
    eng::CharacterController controller{world, spawn};
    game::PlayerState player;
    player.position = spawn;
    game::PlayerState previous_player = player;

    // --- gameplay: weapon, targets, audio ---------------------------------
    auto audio = eng::AudioEngine::create();  // optional: game runs silent if it fails
    const auto sound = [&](const char* name, float volume = 1.0f) {
        if (audio) {
            audio->play(*assets_root / "sounds" / name, volume);
        }
    };

    game::WeaponConfig weapon_config;
    if (const auto text = eng::read_text_file(*assets_root / "weapons/rifle.cfg")) {
        if (const auto parsed = game::parse_weapon_config(*text)) {
            weapon_config = *parsed;
        } else {
            eng::log::warn("Using built-in weapon defaults (config parse failed)");
        }
    }
    game::WeaponState weapon;
    weapon.ammo = weapon_config.magazine_size;

    constexpr float kTargetRadius = 0.4f;
    constexpr float kTargetHeight = 1.8f;
    constexpr float kTargetRespawnSeconds = 3.0f;
    std::vector<Target> targets = {
        {.home = {8.0f, 0.0f, -14.0f}, .position = {}, .health = {50.0f, 50.0f}},
        {.home = {-8.0f, 0.0f, -14.0f}, .position = {}, .health = {50.0f, 50.0f}},
        {.home = {0.0f, 1.5f, 0.0f}, .position = {}, .health = {50.0f, 50.0f}},  // on platform
        {.home = {12.0f, 0.0f, 6.0f},
         .position = {},
         .health = {50.0f, 50.0f},
         .respawn_remaining = 0.0f,
         .patrol_radius = 4.0f},
        {.home = {-12.0f, 0.0f, 6.0f},
         .position = {},
         .health = {50.0f, 50.0f},
         .respawn_remaining = 0.0f,
         .patrol_radius = 4.0f,
         .patrol_phase = 3.14f},
    };
    for (Target& target : targets) {
        target.position = target.home;
    }
    int kills = 0;
    int deaths = 0;
    std::vector<Tracer> tracers;
    double sim_time = 0.0;

    float view_yaw = 0.0f;
    float view_pitch = 0.0f;
    std::uint32_t input_sequence = 0;

    game::FlyCamera fly;
    fly.camera.position = spawn + glm::vec3{0.0f, 8.0f, 12.0f};
    fly.camera.pitch = -0.5f;
    bool fly_mode = false;
    bool draw_physics = true;

    eng::InputState input;
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / game::kTickRate};

    window->set_relative_mouse(true);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_FRAMEBUFFER_SRGB);

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
        if (input.was_pressed(eng::Key::F1)) {
            fly_mode = !fly_mode;
            eng::log::debug("fly mode: {}", fly_mode);
        }
        if (input.was_pressed(eng::Key::F3)) {
            draw_physics = !draw_physics;
        }

        const double dt = clock.tick();

        // View angles update every render frame for minimal aim latency;
        // simulation consumes the latest angles at each fixed tick.
        if (window->relative_mouse() && !fly_mode) {
            constexpr float kSensitivity = 0.002f;
            view_yaw += input.mouse_dx() * kSensitivity;
            view_pitch -= input.mouse_dy() * kSensitivity;
            view_pitch = std::clamp(view_pitch, -eng::Camera::kMaxPitchRadians,
                                    eng::Camera::kMaxPitchRadians);
        }

        const bool reload_requested = input.was_pressed(eng::Key::R);
        bool reload_consumed = false;

        step.advance(dt);
        while (step.consume_tick()) {
            sim_time += game::kTickSeconds;
            previous_player = player;
            if (fly_mode) {
                continue;
            }
            const game::InputCommand command =
                make_command(input, view_yaw, view_pitch, input_sequence++);
            const bool was_on_ground = player.on_ground;
            game::advance_player(player, command, game::kTickSeconds, controller, world);
            if (was_on_ground && !player.on_ground && player.velocity.y > 0.0f) {
                sound("jump.wav", 0.5f);
            }

            // Fell out of the world -> count a death and respawn.
            if (player.position.y < -20.0f) {
                player = {};
                player.position = spawn;
                ++deaths;
                sound("death.wav", 0.8f);
            }

            // --- weapon -------------------------------------------------
            const bool fire_held =
                game::has_button(command, game::Button::Fire) && window->relative_mouse();
            const game::WeaponTickResult shot = game::update_weapon(
                weapon, weapon_config, fire_held, reload_requested && !reload_consumed,
                game::kTickSeconds);
            reload_consumed = true;
            if (shot.reload_started) {
                sound("reload.wav");
            }
            if (shot.dry_fired) {
                sound("dry.wav", 0.7f);
            }
            if (shot.fired) {
                sound("fire.wav", 0.9f);
                const glm::vec3 eye =
                    player.position + glm::vec3{0.0f, game::kMove.eye_height, 0.0f};
                eng::Camera aim;
                aim.yaw = command.yaw;
                aim.pitch = command.pitch;
                const glm::vec3 dir = aim.forward();

                // Wall distance bounds the shot; then find the closest target.
                float max_t = weapon_config.range;
                if (const auto wall = world.raycast(eye, dir, weapon_config.range)) {
                    max_t = wall->distance;
                }
                Target* hit_target = nullptr;
                float hit_t = max_t;
                for (Target& target : targets) {
                    if (!target.alive()) {
                        continue;
                    }
                    const auto t = game::ray_vertical_capsule(eye, dir, target.position,
                                                              kTargetRadius, kTargetHeight);
                    if (t && *t < hit_t) {
                        hit_t = *t;
                        hit_target = &target;
                    }
                }
                tracers.push_back({eye + dir * 0.4f - glm::vec3{0.0f, 0.06f, 0.0f},
                                   eye + dir * hit_t, 0.08f});
                if (hit_target != nullptr) {
                    const float distance = hit_t;
                    const float volume = std::clamp(1.2f - distance / 40.0f, 0.2f, 1.0f);
                    if (game::apply_damage(hit_target->health, weapon_config.damage)) {
                        hit_target->respawn_remaining = kTargetRespawnSeconds;
                        ++kills;
                        sound("kill.wav", volume);
                    } else {
                        sound("hit.wav", volume);
                    }
                }
            }

            // --- targets --------------------------------------------------
            for (std::size_t t = 0; t < targets.size(); ++t) {
                Target& target = targets[t];
                if (!target.alive()) {
                    target.respawn_remaining -= game::kTickSeconds;
                    if (target.alive()) {
                        game::reset_health(target.health);
                        target.position = target.home;
                    }
                    continue;
                }
                if (target.patrol_radius > 0.0f) {
                    const float phase =
                        static_cast<float>(sim_time) * 0.8f + target.patrol_phase;
                    target.position =
                        target.home +
                        glm::vec3{std::sin(phase) * target.patrol_radius, 0.0f, 0.0f};
                }
            }
        }
        for (Tracer& tracer : tracers) {
            tracer.ttl -= static_cast<float>(dt);
        }
        std::erase_if(tracers, [](const Tracer& tracer) { return tracer.ttl <= 0.0f; });
        if (audio) {
            audio->update();
        }

        eng::Camera camera;
        if (fly_mode) {
            fly.update(input, static_cast<float>(dt), window->relative_mouse());
            camera = fly.camera;
        } else {
            const float alpha = static_cast<float>(step.alpha());
            const glm::vec3 eye_pos =
                glm::mix(previous_player.position, player.position, alpha) +
                glm::vec3{0.0f, game::kMove.eye_height, 0.0f};
            camera.position = eye_pos;
            camera.yaw = view_yaw;
            camera.pitch = view_pitch;
        }
        camera.aspect = window->aspect();

        // --- render -----------------------------------------------------
        glViewport(0, 0, window->width_px(), window->height_px());
        glClearColor(0.055f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const glm::mat4 view_projection = camera.view_projection();
        int draw_calls = 0;

        lit_shader->bind();
        lit_shader->set_mat4("u_view_projection", view_projection);
        lit_shader->set_vec3("u_light_direction", glm::normalize(glm::vec3{-0.4f, -1.0f, -0.3f}));
        lit_shader->set_vec3("u_light_color", {1.0f, 0.97f, 0.9f});
        lit_shader->set_vec3("u_ambient", {0.20f, 0.22f, 0.26f});
        lit_shader->set_vec3("u_camera_pos", camera.position);
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

        // Targets: stretched cubes colored by remaining health.
        for (const Target& target : targets) {
            if (!target.alive()) {
                continue;
            }
            glm::mat4 model = glm::translate(
                glm::mat4{1.0f}, target.position + glm::vec3{0.0f, kTargetHeight * 0.5f, 0.0f});
            model = glm::scale(model, {kTargetRadius * 2.0f, kTargetHeight, kTargetRadius * 2.0f});
            lit_shader->set_mat4("u_model", model);
            lit_shader->set_mat3("u_normal_matrix", glm::mat3(glm::transpose(glm::inverse(model))));
            const float hp = target.health.current / target.health.max;
            lit_shader->set_vec3("u_tint", {0.9f, 0.15f + 0.6f * hp, 0.15f});
            cube.draw();
            ++draw_calls;
        }

        for (const Tracer& tracer : tracers) {
            debug_draw->line(tracer.from, tracer.to, {1.0f, 0.9f, 0.4f});
        }

        if (draw_physics) {
            draw_capsule(*debug_draw, player.position, controller.config().radius,
                         controller.config().height,
                         player.on_ground ? glm::vec3{0.2f, 1.0f, 0.3f}
                                          : glm::vec3{1.0f, 0.6f, 0.1f});
            debug_draw->line(player.position, player.position + player.velocity * 0.3f,
                             {1.0f, 1.0f, 0.2f});
            // Aim ray from the eye.
            const glm::vec3 eye = camera.position;
            if (const auto hit = world.raycast(eye, camera.forward(), 100.0f)) {
                debug_draw->line(eye, hit->position, {0.9f, 0.2f, 0.2f});
                debug_draw->line(hit->position, hit->position + hit->normal * 0.5f,
                                 {0.2f, 0.6f, 1.0f});
            }
        }
        debug_draw->axes(glm::mat4{1.0f}, 2.0f);
        draw_calls += debug_draw->flush(view_projection) > 0 ? 1 : 0;

        // --- debug UI -----------------------------------------------------
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
        ImGui::Text("pos: (%.2f, %.2f, %.2f)", player.position.x, player.position.y,
                    player.position.z);
        ImGui::Text("vel: (%.2f, %.2f, %.2f) |h|=%.2f", player.velocity.x, player.velocity.y,
                    player.velocity.z,
                    std::hypot(player.velocity.x, player.velocity.z));
        ImGui::Text("on_ground: %s", player.on_ground ? "yes" : "no");
        ImGui::Checkbox("physics debug (F3)", &draw_physics);
        ImGui::Checkbox("fly mode (F1)", &fly_mode);
        ImGui::TextDisabled("Esc: mouse capture | WASD+Space: move | LMB: fire | R: reload");
        ImGui::End();

        // --- HUD ---------------------------------------------------------
        if (!fly_mode) {
            ImDrawList* overlay = ImGui::GetForegroundDrawList();
            const ImVec2 center{ImGui::GetIO().DisplaySize.x * 0.5f,
                                ImGui::GetIO().DisplaySize.y * 0.5f};
            const ImU32 cross_color = IM_COL32(240, 240, 240, 220);
            constexpr float kGap = 4.0f;
            constexpr float kArm = 9.0f;
            overlay->AddLine({center.x - kGap - kArm, center.y}, {center.x - kGap, center.y},
                             cross_color, 2.0f);
            overlay->AddLine({center.x + kGap, center.y}, {center.x + kGap + kArm, center.y},
                             cross_color, 2.0f);
            overlay->AddLine({center.x, center.y - kGap - kArm}, {center.x, center.y - kGap},
                             cross_color, 2.0f);
            overlay->AddLine({center.x, center.y + kGap}, {center.x, center.y + kGap + kArm},
                             cross_color, 2.0f);

            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos({display.x * 0.5f, display.y - 20.0f}, ImGuiCond_Always,
                                    {0.5f, 1.0f});
            ImGui::Begin("##hud", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
            ImGui::SetWindowFontScale(1.6f);
            if (weapon.reloading()) {
                ImGui::Text("HP 100    RELOADING (%.1fs)    K %d / D %d",
                            weapon.reload_remaining_seconds, kills, deaths);
            } else {
                ImGui::Text("HP 100    %d / %d    K %d / D %d", weapon.ammo,
                            weapon_config.magazine_size, kills, deaths);
            }
            ImGui::End();
        }
        imgui->end_frame();

        window->swap();

#if defined(ENG_ENABLE_ASSERTS)
        eng::check_gl_errors("frame end");
#endif

        if (args.run_seconds && clock.elapsed() >= *args.run_seconds) {
            eng::log::info("--run-seconds elapsed; quitting (pos=({:.2f},{:.2f},{:.2f}))",
                           player.position.x, player.position.y, player.position.z);
            running = false;
        }
    }

    eng::log::info("FPS client shutting down cleanly");
    return 0;
}
