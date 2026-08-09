#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"
#include "engine/core/log.h"
#include "engine/core/time.h"
#include "engine/core/version.h"
#include "engine/net/composite_transport.h"
#include "engine/net/transport.h"
#include "engine/net/websocket_host.h"
#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
#include "game/server/server_game.h"
#include "game/shared/bot.h"
#include "game/shared/player_movement.h"
#include "game/shared/replay.h"
#include "game/shared/weapon.h"

namespace {

struct ServerArgs {
    std::uint16_t port = 7777;             // ENet/UDP (native clients)
    std::optional<std::uint16_t> ws_port;  // WebSocket/TCP (browser clients)
    bool enet = true;                      // --no-enet to run WS-only
    std::optional<double> run_seconds;
    bool verbose = false;
    std::optional<std::string> record_path;             // --record: write a replay
    std::optional<std::string> replay_path;             // --replay: re-simulate one and exit
    int bots = 0;                                       // --bots N: fill N slots with AI
    std::string map = "maps/arena01.glb";               // --map: which arena to host
    game::BotSkill bot_skill = game::BotSkill::Normal;  // --bot-skill
    // --stats PATH: career records that outlive the process. Off by default,
    // because a server that suddenly needs a writable path is a server that
    // suddenly has a new way to fail to start.
    std::optional<std::string> stats_path;
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
        } else if (arg == "--ws-port") {
            if (const auto value = next_value()) {
                std::uint16_t port = 0;
                if (std::from_chars(value->data(), value->data() + value->size(), port).ec ==
                    std::errc{}) {
                    args.ws_port = port;
                }
            }
        } else if (arg == "--no-enet") {
            args.enet = false;
        } else if (arg == "--run-seconds") {
            if (const auto value = next_value()) {
                double seconds = 0.0;
                if (std::from_chars(value->data(), value->data() + value->size(), seconds).ec ==
                    std::errc{}) {
                    args.run_seconds = seconds;
                }
            }
        } else if (arg == "--record") {
            if (i + 1 < argc) {
                args.record_path = argv[++i];
            }
        } else if (arg == "--replay") {
            if (i + 1 < argc) {
                args.replay_path = argv[++i];
            }
        } else if (arg == "--bots") {
            if (const auto value = next_value()) {
                int count = 0;
                if (std::from_chars(value->data(), value->data() + value->size(), count).ec ==
                    std::errc{}) {
                    args.bots = std::clamp(count, 0, static_cast<int>(game::kMaxPlayers));
                }
            }
        } else if (arg == "--map") {
            if (const auto value = next_value()) {
                args.map = std::string{*value};
            }
        } else if (arg == "--stats") {
            if (i + 1 < argc) {
                args.stats_path = argv[++i];
            }
        } else if (arg == "--bot-skill") {
            if (const auto value = next_value()) {
                if (const auto skill = game::bot_skill_from_name(*value)) {
                    args.bot_skill = *skill;
                } else {
                    eng::log::warn("Unknown --bot-skill '{}'; keeping {}", *value,
                                   game::bot_skill_name(args.bot_skill));
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

// Re-simulates a recording headlessly and reports where everyone ended up.
//
// This runs the SAME advance_player the live server runs, on inputs only --
// no positions were recorded. So a replay that lands somewhere different is
// not a replay bug, it is a determinism bug in the simulation, which is
// exactly what makes this worth having as a tool.
int run_replay(const std::filesystem::path& path,
               const std::vector<std::pair<eng::MeshData, glm::mat4>>& collision) {
    const auto replay = game::read_replay_file(path);
    if (!replay) {
        return 1;
    }
    eng::log::info("Replay '{}': map '{}', {} players, {} frames at {} Hz", path.string(),
                   replay->map_path, replay->players.size(), replay->frames.size(),
                   replay->tick_rate_hz);

    eng::PhysicsWorld world;
    for (const auto& [mesh, transform] : collision) {
        world.add_static_mesh(mesh, transform);
    }
    world.optimize();

    struct Simulated {
        game::PlayerState state;
        std::unique_ptr<eng::CharacterController> controller;
    };
    std::array<std::optional<Simulated>, game::kMaxPlayers> players;
    for (const game::ReplayPlayer& recorded : replay->players) {
        Simulated simulated;
        simulated.state.position = recorded.spawn;
        simulated.controller = std::make_unique<eng::CharacterController>(world, recorded.spawn);
        players[recorded.id] = std::move(simulated);
    }

    for (const game::ReplayFrame& frame : replay->frames) {
        for (const game::ReplayCommand& entry : frame.commands) {
            auto& player = players[entry.player_id];
            if (!player) {
                continue;  // a command for a player the header never declared
            }
            game::advance_player(player->state, entry.command, game::kTickSeconds,
                                 *player->controller, world);
        }
    }

    for (const game::ReplayPlayer& recorded : replay->players) {
        const auto& player = players[recorded.id];
        if (!player) {
            continue;
        }
        const glm::vec3& p = player->state.position;
        eng::log::info("  player {} '{}': final pos ({:.4f}, {:.4f}, {:.4f}) on_ground {}",
                       recorded.id, recorded.name, p.x, p.y, p.z, player->state.on_ground);
    }
    eng::log::info("Replay finished");
    return 0;
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
    // The map name is echoed to every client in ServerWelcome, so it is a
    // value this process hands out, not just one it reads. Normalizing and
    // rejecting an escaping path keeps `--map ../../etc/passwd` from becoming
    // a load attempt here or a path the client is told to open.
    const std::string map_path = eng::normalize_asset_path(args.map);
    if (map_path.empty() || eng::asset_path_escapes_root(map_path)) {
        eng::log::error("--map '{}' is not a path inside assets/", args.map);
        return 1;
    }
    // The server needs collision geometry only, so texture pixels are never
    // decoded: that is the expensive part of loading a map.
    eng::AssetCache assets{*assets_root, /*decode_images=*/false};
    const eng::GltfModel* map = assets.model(map_path);
    if (map == nullptr) {
        eng::log::error("Could not load map '{}'", map_path);
        return 1;
    }
    eng::log::info("Map: {}", map_path);

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

    if (args.replay_path) {
        return run_replay(*args.replay_path, collision);
    }

    // Slot order is the arsenal order: 1=rifle, 2=smg, 3=shotgun, 4=sniper.
    game::Arsenal arsenal;
    for (const char* weapon : {"rifle", "smg", "shotgun", "sniper"}) {
        const auto text =
            eng::read_text_file(*assets_root / "weapons" / (std::string(weapon) + ".cfg"));
        if (!text) {
            continue;
        }
        if (const auto parsed = game::parse_weapon_config(*text)) {
            arsenal.weapons.push_back(*parsed);
        } else {
            eng::log::warn("Weapon '{}' failed to parse; skipping", weapon);
        }
    }
    if (arsenal.empty()) {
        eng::log::warn("No weapons loaded; falling back to the built-in rifle");
    }

    // --- transports: ENet (native) and/or WebSocket (browser) -------------
    std::vector<std::unique_ptr<eng::IServerTransport>> transports;
    if (args.enet) {
        auto host = eng::NetHost::create_server(args.port, game::kMaxPlayers);
        if (!host) {
            return 1;
        }
        transports.push_back(std::make_unique<eng::NetHost>(std::move(*host)));
    }
    if (args.ws_port) {
        auto host = eng::WebSocketHost::create_server(*args.ws_port, game::kMaxPlayers);
        if (!host) {
            return 1;
        }
        transports.push_back(std::make_unique<eng::WebSocketHost>(std::move(*host)));
    }
    if (transports.empty()) {
        eng::log::error("No transport enabled (use --no-enet only with --ws-port)");
        return 1;
    }
    eng::CompositeTransport net{std::move(transports)};

    game::ServerGame server{std::move(collision), std::move(spawns), map_path, std::move(arsenal)};
    server.set_bot_config(game::bot_config_for(args.bot_skill));
    if (args.stats_path) {
        server.set_stats_path(*args.stats_path);
    }
    if (args.bots > 0) {
        eng::log::info("Bot skill: {}", game::bot_skill_name(args.bot_skill));
    }
    if (args.record_path) {
        server.start_recording(*args.record_path);
    }
    for (int i = 0; i < args.bots; ++i) {
        if (!server.add_bot("bot" + std::to_string(i + 1))) {
            break;
        }
    }

    // --- fixed-tick headless loop ------------------------------------------
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / game::kTickRate};
    std::vector<eng::NetEvent> events;
    double last_stats_log = 0.0;
    std::uint32_t ticks_at_last_log = 0;

    bool running = true;
    while (running) {
        events.clear();
        net.poll(events);
        for (const eng::NetEvent& event : events) {
            server.handle_event(event, net);
        }

        step.advance(clock.tick());
        while (step.consume_tick()) {
            server.tick(net);
        }

        const double elapsed = clock.elapsed();
        if (elapsed - last_stats_log >= 5.0) {
            const double tick_rate =
                (server.current_tick() - ticks_at_last_log) / (elapsed - last_stats_log);
            eng::log::info("tick {} ({:.1f}/s) | players {} | rx {} B tx {} B",
                           server.current_tick(), tick_rate, server.player_count(),
                           net.stats().bytes_received, net.stats().bytes_sent);
            ticks_at_last_log = server.current_tick();
            last_stats_log = elapsed;
        }

        if (args.run_seconds && elapsed >= *args.run_seconds) {
            running = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Stats are flushed at match end and when a player leaves, but a match
    // runs five minutes and a restart lands wherever it lands -- without this
    // a clean shutdown would drop everything since the last flush, which is
    // most of what happened. Found by playing a match and getting an empty
    // file, not by reasoning about it.
    server.save_stats();

    // Written on the way out rather than incrementally: a match is a few
    // hundred KB of inputs, and a partial file would decode to a truncated
    // match that looks legitimate.
    if (!server.write_replay()) {
        eng::log::error("Failed to write the replay");
        return 1;
    }

    eng::log::info("FPS dedicated server shutting down cleanly");
    return 0;
}
