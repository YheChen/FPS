#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// Minimal logging facility.
//
// Design notes:
//  - This is the one sanctioned piece of global state in the engine (the
//    minimum log level). Logging from anywhere is more valuable than purity
//    here; see docs/coding-standards.md.
//  - Thread-safe: concurrent writes never interleave within a line.
//  - Timestamps are UTC time-of-day. Deterministic to format, no locale or
//    timezone dependencies.
//  - Output: Trace/Debug/Info go to stdout, Warn/Error go to stderr.
namespace eng::log {

enum class Level : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

// The fixed-width tag used in formatted output, e.g. "INFO ".
std::string_view level_tag(Level level);

// Global minimum level. Messages below it are dropped. Defaults to Info.
void set_level(Level level);
Level level();

// True if a message at `level` would currently be emitted.
bool should_log(Level level);

// Formats one complete log line: "[HH:MM:SS.mmm] [TAG  ] message".
// Pure function (no global state, no I/O) so it is unit-testable.
std::string format_line(Level level, std::string_view message,
                        std::chrono::system_clock::time_point timestamp);

// Writes an already-formatted message at the given level, stamped with the
// current time. Thread-safe.
void write(Level level, std::string_view message);

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (should_log(Level::Trace)) {
        write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (should_log(Level::Debug)) {
        write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (should_log(Level::Info)) {
        write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (should_log(Level::Warn)) {
        write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (should_log(Level::Error)) {
        write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
    }
}

}  // namespace eng::log
