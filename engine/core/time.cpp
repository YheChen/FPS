#include "engine/core/time.h"

#include <algorithm>

#include "engine/core/assert.h"

namespace eng {

namespace {

double seconds_between(std::chrono::steady_clock::time_point from,
                       std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

}  // namespace

Clock::Clock() : start_(std::chrono::steady_clock::now()), last_(start_) {}

double Clock::tick() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = seconds_between(last_, now);
    last_ = now;
    return std::min(dt, kMaxFrameSeconds);
}

double Clock::elapsed() const {
    return seconds_between(start_, std::chrono::steady_clock::now());
}

FixedTimestep::FixedTimestep(double tick_seconds) : tick_seconds_(tick_seconds) {
    ENG_ASSERT(tick_seconds > 0.0, "tick length must be positive");
}

void FixedTimestep::advance(double frame_seconds) {
    ENG_ASSERT(frame_seconds >= 0.0, "frame time cannot be negative");
    accumulator_ += frame_seconds;
    const double cap = tick_seconds_ * static_cast<double>(kMaxPendingTicks);
    accumulator_ = std::min(accumulator_, cap);
}

bool FixedTimestep::consume_tick() {
    if (accumulator_ < tick_seconds_) {
        return false;
    }
    accumulator_ -= tick_seconds_;
    ++tick_count_;
    return true;
}

double FixedTimestep::alpha() const {
    return accumulator_ / tick_seconds_;
}

}  // namespace eng
