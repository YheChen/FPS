#include "engine/core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace eng::log {
namespace {

std::atomic<Level> g_min_level{Level::Info};

// Serializes writes so lines from different threads never interleave.
std::mutex g_write_mutex;

}  // namespace

std::string_view level_tag(Level level) {
    switch (level) {
        case Level::Trace:
            return "TRACE";
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO ";
        case Level::Warn:
            return "WARN ";
        case Level::Error:
            return "ERROR";
        case Level::Off:
            return "OFF  ";
    }
    return "?????";
}

void set_level(Level level) {
    g_min_level.store(level, std::memory_order_relaxed);
}

Level level() {
    return g_min_level.load(std::memory_order_relaxed);
}

bool should_log(Level level) {
    return level != Level::Off && level >= g_min_level.load(std::memory_order_relaxed);
}

std::string format_line(Level level, std::string_view message,
                        std::chrono::system_clock::time_point timestamp) {
    using namespace std::chrono;
    const auto since_epoch = timestamp.time_since_epoch();
    const auto h = duration_cast<hours>(since_epoch) % 24;
    const auto m = duration_cast<minutes>(since_epoch) % 60;
    const auto s = duration_cast<seconds>(since_epoch) % 60;
    const auto ms = duration_cast<milliseconds>(since_epoch) % 1000;
    return std::format("[{:02}:{:02}:{:02}.{:03}] [{}] {}", h.count(), m.count(), s.count(),
                       ms.count(), level_tag(level), message);
}

void write(Level level, std::string_view message) {
    if (!should_log(level)) {
        return;
    }
    const std::string line = format_line(level, message, std::chrono::system_clock::now());
    std::FILE* stream = (level >= Level::Warn) ? stderr : stdout;

    const std::lock_guard<std::mutex> lock(g_write_mutex);
    std::fputs(line.c_str(), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

}  // namespace eng::log
