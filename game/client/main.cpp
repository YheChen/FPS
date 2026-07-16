#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <numbers>
#include <optional>
#include <string>
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
#include "game/client/net_client.h"
#include "game/shared/health.h"
#include "game/shared/interpolation.h"
#include "game/shared/prediction.h"
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
    std::optional<std::string> connect_host;  // online mode when set
    std::uint16_t port = 7777;
    std::string name = "player";
    bool vsync = true;  // --no-vsync: automated multi-window runs must not
                        // block on swap (macOS throttles occluded windows)
    eng::NetSimConfig net_sim;  // --fake-latency/--fake-jitter/--fake-loss
    // Automated-verification hooks (harmless in normal play):
    bool auto_fire = false;                 // hold the trigger every tick
    std::optional<float> fixed_yaw;         // lock the view yaw (radians)
};

ClientArgs parse_args(int argc, char** argv) {
    ClientArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next_value = [&]() -> std::optional<std::string_view> {
            if (i + 1 < argc) {
                return std::string_view{argv[++i]};
            }
            return std::nullopt;
        };
        if (arg == "--run-seconds") {
            if (const auto value = next_value()) {
                double seconds = 0.0;
                if (std::from_chars(value->data(), value->data() + value->size(), seconds).ec ==
                    std::errc{}) {
                    args.run_seconds = seconds;
                }
            }
        } else if (arg == "--connect") {
            if (const auto value = next_value()) {
                args.connect_host = std::string(*value);
            }
        } else if (arg == "--port") {
            if (const auto value = next_value()) {
                std::uint16_t port = 0;
                if (std::from_chars(value->data(), value->data() + value->size(), port).ec ==
                    std::errc{}) {
                    args.port = port;
                }
            }
        } else if (arg == "--name") {
            if (const auto value = next_value()) {
                args.name = std::string(*value);
            }
        } else if (arg == "--no-vsync") {
            args.vsync = false;
        } else if (arg == "--auto-fire") {
            args.auto_fire = true;
        } else if (arg == "--fixed-yaw") {
            if (const auto value = next_value()) {
                float yaw = 0.0f;
                if (std::from_chars(value->data(), value->data() + value->size(), yaw).ec ==
                    std::errc{}) {
                    args.fixed_yaw = yaw;
                }
            }
        } else if (arg == "--fake-latency") {
            if (const auto value = next_value()) {
                std::from_chars(value->data(), value->data() + value->size(),
                                args.net_sim.latency_ms);
            }
        } else if (arg == "--fake-jitter") {
            if (const auto value = next_value()) {
                std::from_chars(value->data(), value->data() + value->size(),
                                args.net_sim.jitter_ms);
            }
        } else if (arg == "--fake-loss") {
            if (const auto value = next_value()) {
                std::from_chars(value->data(), value->data() + value->size(),
                                args.net_sim.loss_percent);
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

struct KillFeedEntry {
    std::string text;
    float ttl;
};

// User preferences persisted next to the executable's working directory.
struct Settings {
    float sensitivity = 0.002f;  // radians per pixel
    float fov_degrees = 70.0f;
    float volume = 1.0f;
    std::string name = "player";
    std::string last_ip = "127.0.0.1";
};

constexpr const char* kSettingsFile = "fps_settings.cfg";

Settings load_settings() {
    Settings s;
    const auto text = eng::read_text_file(kSettingsFile);
    if (!text) {
        return s;  // first run
    }
    std::string_view rest = *text;
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = (nl == std::string_view::npos) ? std::string_view{} : rest.substr(nl + 1);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        const auto to_float = [&](float& out) {
            std::from_chars(value.data(), value.data() + value.size(), out);
        };
        if (key == "sensitivity") {
            to_float(s.sensitivity);
        } else if (key == "fov") {
            to_float(s.fov_degrees);
        } else if (key == "volume") {
            to_float(s.volume);
        } else if (key == "name" && !value.empty() && value.size() <= game::kMaxNameLength) {
            s.name = std::string(value);
        } else if (key == "last_ip" && !value.empty() && value.size() < 64) {
            s.last_ip = std::string(value);
        }
    }
    s.sensitivity = std::clamp(s.sensitivity, 0.0002f, 0.02f);
    s.fov_degrees = std::clamp(s.fov_degrees, 50.0f, 120.0f);
    s.volume = std::clamp(s.volume, 0.0f, 1.0f);
    return s;
}

void save_settings(const Settings& s) {
    std::string out = std::format("sensitivity={}\nfov={}\nvolume={}\nname={}\nlast_ip={}\n",
                                  s.sensitivity, s.fov_degrees, s.volume, s.name, s.last_ip);
    std::ofstream file(kSettingsFile, std::ios::binary | std::ios::trunc);
    if (file) {
        file << out;
    } else {
        eng::log::warn("Could not save settings to {}", kSettingsFile);
    }
}

enum class Mode : std::uint8_t {
    Menu,     // main menu over an orbiting arena view
    Offline,  // practice range (targets)
    Online,   // connected to a server
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

    auto window =
        eng::Window::create({.title = "FPS", .width = 1280, .height = 720, .vsync = args.vsync});
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

    // --- settings, menu state, networking ---------------------------------
    Settings settings = load_settings();
    if (args.name != "player") {
        settings.name = args.name;  // CLI overrides the saved name
    }
    if (audio) {
        audio->set_master_volume(settings.volume);
    }

    std::optional<game::NetClient> net;
    std::optional<game::Prediction> prediction;
    Mode mode = Mode::Menu;
    std::string menu_error;
    char menu_name[17]{};
    char menu_ip[64]{};
    std::snprintf(menu_name, sizeof(menu_name), "%s", settings.name.c_str());
    std::snprintf(menu_ip, sizeof(menu_ip), "%s", settings.last_ip.c_str());

    if (args.connect_host) {
        net = game::NetClient::connect(*args.connect_host, args.port, settings.name);
        if (!net) {
            eng::log::error("Failed to start network client");
            return 1;
        }
        // The client predicts on its OWN physics world with the shared
        // movement code; the server remains authoritative.
        prediction.emplace(world, spawn);
        mode = Mode::Online;
        if (args.net_sim.enabled()) {
            eng::log::warn("Network simulation active: {} ms +{} ms jitter, {:.1f}% loss",
                           args.net_sim.latency_ms, args.net_sim.jitter_ms,
                           args.net_sim.loss_percent);
        }
    } else if (args.run_seconds) {
        mode = Mode::Offline;  // automated runs go straight to the range
    }
    bool online = mode == Mode::Online;
    std::deque<game::InputCommand> recent_commands;  // newest first
    std::uint32_t client_tick = 0;
    double remote_render_tick = -1.0;  // fractional server tick remote players render at
    std::array<float, 240> prediction_error_history{};
    std::size_t prediction_error_cursor = 0;
    float last_reconcile_error = 0.0f;
    eng::NetSimConfig sim_config = args.net_sim;
    std::deque<KillFeedEntry> kill_feed;
    const auto player_name = [&](std::uint8_t id) -> std::string {
        if (!online) {
            return "?";
        }
        const auto it = net->players().find(id);
        return it != net->players().end() ? it->second.name : "world";
    };

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

    window->set_relative_mouse(mode != Mode::Menu);

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
        if (input.was_pressed(eng::Key::Escape) && mode != Mode::Menu) {
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
        if (window->relative_mouse() && !fly_mode && mode != Mode::Menu) {
            view_yaw += input.mouse_dx() * settings.sensitivity;
            view_pitch -= input.mouse_dy() * settings.sensitivity;
            view_pitch = std::clamp(view_pitch, -eng::Camera::kMaxPitchRadians,
                                    eng::Camera::kMaxPitchRadians);
        }

        const bool reload_requested = input.was_pressed(eng::Key::R);
        bool reload_consumed = false;

        if (online) {
            net->set_simulation(sim_config);
            net->poll();
            // Server gone or refused us: back to the menu.
            if (net->state() == game::NetClient::State::Disconnected ||
                net->state() == game::NetClient::State::Rejected) {
                menu_error = net->state() == game::NetClient::State::Rejected
                                 ? "server rejected the connection"
                                 : "disconnected from server";
                net.reset();
                prediction.reset();
                online = false;
                mode = Mode::Menu;
                kill_feed.clear();
                recent_commands.clear();
                remote_render_tick = -1.0;
                window->set_relative_mouse(false);
            }
        }

        step.advance(dt);
        while (step.consume_tick()) {
            sim_time += game::kTickSeconds;
            ++client_tick;
            previous_player = player;
            if (fly_mode || mode == Mode::Menu) {
                continue;
            }

            if (online) {
                // Predict locally with the shared movement code; the server
                // corrects us via reconcile() below. While dead, inputs keep
                // flowing (so acks stay in sync) but nothing is predicted.
                if (args.fixed_yaw) {
                    view_yaw = *args.fixed_yaw;
                }
                game::InputCommand command =
                    make_command(input, view_yaw, view_pitch, input_sequence++);
                if (!window->relative_mouse() && !args.auto_fire) {
                    game::set_button(command, game::Button::Fire, false);  // menu clicks
                }
                if (args.auto_fire) {
                    game::set_button(command, game::Button::Fire, true);
                }
                if (net->self_alive()) {
                    prediction->tick(command);
                }
                recent_commands.push_front(command);
                if (recent_commands.size() > 8) {
                    recent_commands.pop_back();
                }
                net->send_input(recent_commands, client_tick,
                                remote_render_tick > 0.0
                                    ? static_cast<std::uint32_t>(remote_render_tick)
                                    : 0u);
                player = prediction->state();
                previous_player = prediction->previous_state();
                continue;  // offline gameplay (targets/weapon) stays offline
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

        if (online) {
            // Combat events -> visuals and audio.
            const glm::vec3 listener =
                player.position + glm::vec3{0.0f, game::kMove.eye_height, 0.0f};
            for (const auto& fire : net->take_fire_events()) {
                tracers.push_back({fire.from + glm::vec3{0.0f, -0.06f, 0.0f}, fire.to, 0.08f});
                const float distance = glm::length(fire.from - listener);
                sound("fire.wav", std::clamp(1.1f - distance / 40.0f, 0.15f, 0.9f));
                if (fire.shooter == net->my_id() && fire.hit_player != game::kNoPlayer) {
                    sound("hit.wav", 0.8f);  // hit confirm
                }
            }
            for (const auto& death : net->take_death_events()) {
                kill_feed.push_back(
                    {player_name(death.killer) + " killed " + player_name(death.victim), 5.0f});
                if (kill_feed.size() > 5) {
                    kill_feed.pop_front();
                }
                if (death.victim == net->my_id()) {
                    sound("death.wav", 0.9f);
                } else if (death.killer == net->my_id()) {
                    sound("kill.wav", 0.8f);
                }
            }
            for (const auto& respawn : net->take_respawn_events()) {
                if (respawn.player == net->my_id()) {
                    prediction->reset(respawn.position);
                    player = prediction->state();
                    previous_player = player;
                }
            }
            for (KillFeedEntry& entry : kill_feed) {
                entry.ttl -= static_cast<float>(dt);
            }
            std::erase_if(kill_feed, [](const KillFeedEntry& e) { return e.ttl <= 0.0f; });

            // Reconciliation: rewind to the authoritative state and replay
            // unacked inputs.
            if (const auto ack = net->take_self_ack()) {
                const auto result = prediction->reconcile(ack->position, ack->velocity,
                                                          ack->on_ground,
                                                          ack->last_processed_input);
                last_reconcile_error = result.error_meters;
                prediction_error_history[prediction_error_cursor] = result.error_meters;
                prediction_error_cursor =
                    (prediction_error_cursor + 1) % prediction_error_history.size();
                player = prediction->state();
                previous_player = prediction->previous_state();
                if (net->server_tick() % 300 < game::kSnapshotDivisor) {
                    eng::log::debug(
                        "net: rtt={}ms pred_err={:.4f}m pending={} interp_buffer_tick={:.1f}",
                        net->rtt_ms(), result.error_meters, prediction->pending().size(),
                        remote_render_tick);
                }
            }
            prediction->update_smoothing(static_cast<float>(dt));

            // Remote render time trails the newest snapshot; advance at tick
            // rate and gently slew toward the target to absorb jitter.
            const double target_tick =
                static_cast<double>(net->server_tick()) - game::kInterpolationDelayTicks;
            if (remote_render_tick < 0.0) {
                remote_render_tick = target_tick;
            } else {
                remote_render_tick += dt * game::kTickRate;
                remote_render_tick += (target_tick - remote_render_tick) * std::min(1.0, dt * 4.0);
            }
        }

        eng::Camera camera;
        if (mode == Mode::Menu) {
            // Slow orbit around the arena behind the menu.
            const float angle = static_cast<float>(clock.elapsed()) * 0.15f;
            camera.position = {std::sin(angle) * 24.0f, 12.0f, std::cos(angle) * 24.0f};
            const glm::vec3 to_center = glm::normalize(-camera.position);
            camera.yaw = std::atan2(to_center.x, -to_center.z);
            camera.pitch = std::asin(to_center.y);
        } else if (fly_mode) {
            fly.update(input, static_cast<float>(dt), window->relative_mouse());
            camera = fly.camera;
        } else {
            const float alpha = static_cast<float>(step.alpha());
            glm::vec3 eye_pos = glm::mix(previous_player.position, player.position, alpha) +
                                glm::vec3{0.0f, game::kMove.eye_height, 0.0f};
            if (online) {
                eye_pos += prediction->smoothing_offset();
            }
            camera.position = eye_pos;
            camera.yaw = view_yaw;
            camera.pitch = view_pitch;
        }
        camera.aspect = window->aspect();
        camera.fov_y_degrees = settings.fov_degrees;

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

        // Remote players (online): interpolated ~100 ms in the past.
        if (online) {
            for (const auto& [id, remote] : net->players()) {
                if (id == net->my_id() || remote.history.empty()) {
                    continue;
                }
                const auto pose = remote.history.sample(remote_render_tick);
                if (!pose || (pose->flags & game::kFlagAlive) == 0) {
                    continue;  // dead players are not drawn
                }
                glm::mat4 model = glm::translate(glm::mat4{1.0f},
                                                 pose->position + glm::vec3{0.0f, 0.9f, 0.0f});
                model = glm::scale(model, {0.8f, 1.8f, 0.8f});
                lit_shader->set_mat4("u_model", model);
                lit_shader->set_mat3("u_normal_matrix",
                                     glm::mat3(glm::transpose(glm::inverse(model))));
                lit_shader->set_vec3("u_tint", {0.3f + 0.2f * static_cast<float>(id % 4),
                                                0.4f, 0.9f - 0.2f * static_cast<float>(id % 4)});
                cube.draw();
                ++draw_calls;
            }
        }

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

        // --- main menu ---------------------------------------------------
        if (mode == Mode::Menu) {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos({display_size.x * 0.5f, display_size.y * 0.45f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({420, 0}, ImGuiCond_Always);
            ImGui::Begin("FPS", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse);
            ImGui::InputText("name", menu_name, sizeof(menu_name));
            ImGui::InputText("server address", menu_ip, sizeof(menu_ip));

            const auto start_session = [&](Mode next) {
                settings.name = menu_name[0] != '\0' ? menu_name : "player";
                settings.last_ip = menu_ip;
                save_settings(settings);
                menu_error.clear();
                player = {};
                player.position = spawn;
                previous_player = player;
                controller.set_position(spawn);
                controller.set_velocity({0.0f, 0.0f, 0.0f});
                view_yaw = 0.0f;
                view_pitch = 0.0f;
                input_sequence = 0;
                mode = next;
                online = next == Mode::Online;
                window->set_relative_mouse(true);
            };

            if (ImGui::Button("Connect", {200, 0})) {
                net = game::NetClient::connect(menu_ip, args.port, menu_name);
                if (net) {
                    prediction.emplace(world, spawn);
                    start_session(Mode::Online);
                } else {
                    menu_error = "could not start a connection (bad address?)";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Practice offline", {200, 0})) {
                start_session(Mode::Offline);
            }
            if (!menu_error.empty()) {
                ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", menu_error.c_str());
            }

            ImGui::SeparatorText("settings");
            ImGui::SliderFloat("sensitivity", &settings.sensitivity, 0.0005f, 0.01f, "%.4f");
            ImGui::SliderFloat("field of view", &settings.fov_degrees, 50.0f, 120.0f, "%.0f");
            if (ImGui::SliderFloat("volume", &settings.volume, 0.0f, 1.0f) && audio) {
                audio->set_master_volume(settings.volume);
            }

            ImGui::Separator();
            if (ImGui::Button("Quit")) {
                running = false;
            }
            ImGui::End();
        }

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
        if (online) {
            ImGui::Separator();
            ImGui::Text("net: %s | id %u | rtt %u ms", net->state_name(), net->my_id(),
                        net->rtt_ms());
            ImGui::Text("server tick %u | acked input %u | pending %zu", net->server_tick(),
                        net->last_processed_input(), prediction->pending().size());
            ImGui::Text("players %zu | rx %llu B tx %llu B", net->players().size(),
                        static_cast<unsigned long long>(net->stats().bytes_received),
                        static_cast<unsigned long long>(net->stats().bytes_sent));
            ImGui::Text("prediction error: %.4f m", last_reconcile_error);
            ImGui::PlotLines("pred err", prediction_error_history.data(),
                             static_cast<int>(prediction_error_history.size()),
                             static_cast<int>(prediction_error_cursor), nullptr, 0.0f, 0.5f,
                             {220, 50});
            if (ImGui::CollapsingHeader("network simulation")) {
                ImGui::SliderInt("latency ms (one-way)", &sim_config.latency_ms, 0, 300);
                ImGui::SliderInt("jitter ms", &sim_config.jitter_ms, 0, 100);
                ImGui::SliderFloat("loss %%", &sim_config.loss_percent, 0.0f, 30.0f);
            }
        }
        ImGui::Checkbox("physics debug (F3)", &draw_physics);
        ImGui::Checkbox("fly mode (F1)", &fly_mode);
        ImGui::TextDisabled("Esc: mouse capture | WASD+Space: move | LMB: fire | R: reload");
        ImGui::End();

        // --- match UI (online) ---------------------------------------------
        if (online) {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;

            // Match timer, top center.
            ImGui::SetNextWindowPos({display_size.x * 0.5f, 8.0f}, ImGuiCond_Always, {0.5f, 0.0f});
            ImGui::Begin("##timer", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
            ImGui::SetWindowFontScale(1.5f);
            ImGui::Text("%02u:%02u", net->match_seconds() / 60, net->match_seconds() % 60);
            ImGui::End();

            // Kill feed, top right.
            if (!kill_feed.empty()) {
                ImGui::SetNextWindowPos({display_size.x - 12.0f, 12.0f}, ImGuiCond_Always,
                                        {1.0f, 0.0f});
                ImGui::Begin("##killfeed", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
                for (const KillFeedEntry& entry : kill_feed) {
                    ImGui::TextColored({1.0f, 0.85f, 0.4f, std::min(1.0f, entry.ttl)}, "%s",
                                       entry.text.c_str());
                }
                ImGui::End();
            }

            // Scoreboard: held Tab, or automatically on the end screen.
            const bool match_over = net->match_phase() == game::MatchPhase::Ended;
            if (input.is_down(eng::Key::Tab) || match_over) {
                ImGui::SetNextWindowPos({display_size.x * 0.5f, display_size.y * 0.35f},
                                        ImGuiCond_Always, {0.5f, 0.5f});
                ImGui::Begin("Scoreboard", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs);
                if (match_over) {
                    ImGui::SetWindowFontScale(1.4f);
                    ImGui::Text("MATCH OVER - restarting in %us", net->match_seconds());
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::Separator();
                }
                // Sort by kills descending.
                std::vector<std::pair<std::uint8_t, game::NetClient::Scores>> rows(
                    net->scores().begin(), net->scores().end());
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                    return a.second.kills > b.second.kills;
                });
                if (ImGui::BeginTable("scores", 3, ImGuiTableFlags_Borders)) {
                    ImGui::TableSetupColumn("player");
                    ImGui::TableSetupColumn("kills");
                    ImGui::TableSetupColumn("deaths");
                    ImGui::TableHeadersRow();
                    for (const auto& [id, score] : rows) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%s%s", player_name(id).c_str(),
                                    id == net->my_id() ? " (you)" : "");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%u", score.kills);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%u", score.deaths);
                    }
                    ImGui::EndTable();
                }
                ImGui::End();
            }

            // Death overlay.
            if (!net->self_alive() && !match_over) {
                ImGui::SetNextWindowPos({display_size.x * 0.5f, display_size.y * 0.5f},
                                        ImGuiCond_Always, {0.5f, 0.5f});
                ImGui::Begin("##dead", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
                ImGui::SetWindowFontScale(2.0f);
                ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "YOU DIED");
                ImGui::End();
            }
        }

        // --- HUD ---------------------------------------------------------
        if (!fly_mode && mode != Mode::Menu) {
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
            if (online) {
                const auto self_score = net->scores().find(net->my_id());
                const int net_kills =
                    self_score != net->scores().end() ? self_score->second.kills : 0;
                const int net_deaths =
                    self_score != net->scores().end() ? self_score->second.deaths : 0;
                if (net->self_reloading()) {
                    ImGui::Text("HP %.0f    RELOADING    K %d / D %d", net->self_health(),
                                net_kills, net_deaths);
                } else {
                    ImGui::Text("HP %.0f    %u / %d    K %d / D %d", net->self_health(),
                                net->self_ammo(), weapon_config.magazine_size, net_kills,
                                net_deaths);
                }
            } else if (weapon.reloading()) {
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

    settings.name = menu_name[0] != '\0' ? menu_name : settings.name;
    settings.last_ip = menu_ip[0] != '\0' ? menu_ip : settings.last_ip;
    save_settings(settings);
    eng::log::info("FPS client shutting down cleanly");
    return 0;
}
