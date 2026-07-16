#include "engine/core/log.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace {

using eng::log::Level;

TEST_CASE("format_line produces timestamp, tag, and message", "[log]") {
    using namespace std::chrono;
    // 13:05:07.042 UTC on day zero of the epoch.
    const auto timestamp = std::chrono::system_clock::time_point{hours{13} + minutes{5} +
                                                                 seconds{7} + milliseconds{42}};

    const std::string line = eng::log::format_line(Level::Info, "hello world", timestamp);

    CHECK(line == "[13:05:07.042] [INFO ] hello world");
}

TEST_CASE("format_line wraps at day boundaries", "[log]") {
    using namespace std::chrono;
    // 25 hours past epoch is 01:00:00.000 the next day.
    const auto timestamp = std::chrono::system_clock::time_point{hours{25}};

    const std::string line = eng::log::format_line(Level::Error, "x", timestamp);

    CHECK(line == "[01:00:00.000] [ERROR] x");
}

TEST_CASE("level tags are fixed width", "[log]") {
    CHECK(eng::log::level_tag(Level::Trace).size() == 5);
    CHECK(eng::log::level_tag(Level::Debug).size() == 5);
    CHECK(eng::log::level_tag(Level::Info).size() == 5);
    CHECK(eng::log::level_tag(Level::Warn).size() == 5);
    CHECK(eng::log::level_tag(Level::Error).size() == 5);
}

TEST_CASE("minimum level filters lower levels", "[log]") {
    const Level previous = eng::log::level();

    eng::log::set_level(Level::Warn);
    CHECK_FALSE(eng::log::should_log(Level::Trace));
    CHECK_FALSE(eng::log::should_log(Level::Debug));
    CHECK_FALSE(eng::log::should_log(Level::Info));
    CHECK(eng::log::should_log(Level::Warn));
    CHECK(eng::log::should_log(Level::Error));

    eng::log::set_level(Level::Trace);
    CHECK(eng::log::should_log(Level::Trace));

    // Off is never emitted, regardless of the minimum level.
    CHECK_FALSE(eng::log::should_log(Level::Off));

    eng::log::set_level(previous);
}

}  // namespace
