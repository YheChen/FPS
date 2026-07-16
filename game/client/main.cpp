#include <glad/gl.h>

#include <charconv>
#include <cstring>
#include <optional>
#include <string_view>

#include "engine/core/log.h"
#include "engine/core/time.h"
#include "engine/core/version.h"
#include "engine/platform/input.h"
#include "engine/platform/window.h"

namespace {

constexpr double kTickRate = 60.0;

struct ClientArgs {
    // When set, the client quits automatically after this many seconds.
    // Used by CI / automated verification.
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

}  // namespace

int main(int argc, char** argv) {
    eng::log::set_level(eng::log::Level::Debug);
    eng::log::info("FPS client starting (engine v{})", eng::version_string());

    const ClientArgs args = parse_args(argc, argv);

    auto window = eng::Window::create({.title = "FPS", .width = 1280, .height = 720});
    if (!window) {
        eng::log::error("Failed to create window; exiting");
        return 1;
    }

    eng::InputState input;
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / kTickRate};

    // FPS counter state.
    int frames_this_second = 0;
    double fps_window_start = 0.0;

    bool running = true;
    while (running) {
        if (!window->poll(input)) {
            running = false;
        }
        if (input.was_pressed(eng::Key::Escape)) {
            window->set_relative_mouse(!window->relative_mouse());
            eng::log::debug("Relative mouse: {}", window->relative_mouse());
        }

        const double dt = clock.tick();
        step.advance(dt);
        while (step.consume_tick()) {
            // Simulation goes here in later milestones. Milestone 1 only
            // proves the fixed-tick loop runs at the configured rate.
        }

        glViewport(0, 0, window->width_px(), window->height_px());
        glClearColor(0.10f, 0.16f, 0.22f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        window->swap();

        ++frames_this_second;
        const double elapsed = clock.elapsed();
        if (elapsed - fps_window_start >= 1.0) {
            eng::log::info("fps={} ticks_total={} alpha={:.2f}", frames_this_second,
                           step.tick_count(), step.alpha());
            frames_this_second = 0;
            fps_window_start = elapsed;
        }
        if (args.run_seconds && elapsed >= *args.run_seconds) {
            eng::log::info("--run-seconds elapsed; quitting");
            running = false;
        }
    }

    eng::log::info("FPS client shutting down cleanly");
    return 0;
}
