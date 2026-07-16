#include <charconv>
#include <chrono>
#include <optional>
#include <string_view>
#include <thread>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"
#include "engine/core/log.h"
#include "engine/core/time.h"
#include "engine/core/version.h"
#include "engine/net/transport.h"
#include "game/server/server_game.h"
#include "game/shared/player_movement.h"
#include "game/shared/weapon.h"

namespace {

struct ServerArgs {
    std::uint16_t port = 7777;
    std::optional<double> run_seconds;
    bool verbose = false;
};

ServerArgs parse_args(int argc, char** argv) {
    ServerArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next_value = [&]() -> std::optional<std::string_view> {
            if (i + 1 < argc) {
                return std::string_view{argv[++i]};
            }
            return std::nullopt;
        };
        if (arg == "--port") {
            if (const auto value = next_value()) {
                std::uint16_t port = 0;
                if (std::from_chars(value->data(), value->data() + value->size(), port).ec ==
                    std::errc{}) {
                    args.port = port;
                }
            }
        } else if (arg == "--run-seconds") {
            if (const auto value = next_value()) {
                double seconds = 0.0;
                if (std::from_chars(value->data(), value->data() + value->size(), seconds).ec ==
                    std::errc{}) {
                    args.run_seconds = seconds;
                }
            }
        } else if (arg == "--verbose") {
            args.verbose = true;
        } else {
            eng::log::warn("Unknown argument '{}'", arg);
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    eng::log::set_level(eng::log::Level::Info);
    eng::log::info("FPS dedicated server starting (engine v{})", eng::version_string());

    const ServerArgs args = parse_args(argc, argv);
    if (args.verbose) {
        eng::log::set_level(eng::log::Level::Debug);
    }

    // --- load map collision (headless) ------------------------------------
    const auto assets_root = eng::find_assets_root();
    if (!assets_root) {
        eng::log::error("Could not locate the assets/ directory");
        return 1;
    }
    constexpr const char* kMapPath = "maps/arena01.glb";
    eng::AssetCache assets{*assets_root};
    const eng::GltfModel* map = assets.model(kMapPath);
    if (map == nullptr) {
        return 1;
    }

    std::vector<std::pair<eng::MeshData, glm::mat4>> collision;
    std::vector<glm::vec3> spawns;
    for (const eng::GltfNode& node : map->nodes) {
        if (node.name.starts_with("spawn_")) {
            spawns.emplace_back(node.transform[3]);
        }
        if (node.mesh < 0) {
            continue;
        }
        for (const eng::GltfPrimitive& primitive :
             map->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
            collision.emplace_back(primitive.mesh, node.transform);
        }
    }

    game::WeaponConfig weapon_config;
    if (const auto text = eng::read_text_file(*assets_root / "weapons/rifle.cfg")) {
        if (const auto parsed = game::parse_weapon_config(*text)) {
            weapon_config = *parsed;
        } else {
            eng::log::warn("Weapon config parse failed; using built-in defaults");
        }
    }

    auto net = eng::NetHost::create_server(args.port, game::kMaxPlayers);
    if (!net) {
        return 1;
    }

    game::ServerGame server{std::move(collision), std::move(spawns), kMapPath, weapon_config};

    // --- fixed-tick headless loop ------------------------------------------
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / game::kTickRate};
    std::vector<eng::NetEvent> events;
    double last_stats_log = 0.0;
    std::uint32_t ticks_at_last_log = 0;

    bool running = true;
    while (running) {
        events.clear();
        net->poll(events);
        for (const eng::NetEvent& event : events) {
            server.handle_event(event, *net);
        }

        step.advance(clock.tick());
        while (step.consume_tick()) {
            server.tick(*net);
        }

        const double elapsed = clock.elapsed();
        if (elapsed - last_stats_log >= 5.0) {
            const double tick_rate =
                (server.current_tick() - ticks_at_last_log) / (elapsed - last_stats_log);
            eng::log::info("tick {} ({:.1f}/s) | players {} | rx {} B tx {} B",
                           server.current_tick(), tick_rate, server.player_count(),
                           net->stats().bytes_received, net->stats().bytes_sent);
            ticks_at_last_log = server.current_tick();
            last_stats_log = elapsed;
        }

        if (args.run_seconds && elapsed >= *args.run_seconds) {
            running = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    eng::log::info("FPS dedicated server shutting down cleanly");
    return 0;
}
