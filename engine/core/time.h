#pragma once

#include <chrono>
#include <cstdint>

namespace eng {

// Wall-clock frame timer. Monotonic (steady_clock); never goes backwards.
class Clock {
public:
    Clock();

    // Seconds since the previous tick() call (or construction), clamped to
    // kMaxFrameSeconds so a debugger pause or hitch cannot explode the
    // simulation.
    double tick();

    // Seconds since construction.
    double elapsed() const;

    static constexpr double kMaxFrameSeconds = 0.25;

private:
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_;
};

// Fixed-timestep accumulator (the classic "fix your timestep" pattern).
//
// Usage per frame:
//   step.advance(frame_dt);
//   while (step.consume_tick()) { simulate(step.tick_seconds()); }
//   render(interpolate(prev, curr, step.alpha()));
//
// The accumulator is capped at kMaxPendingTicks ticks so a long stall
// degrades (simulation slows down) instead of spiraling.
class FixedTimestep {
public:
    explicit FixedTimestep(double tick_seconds);

    void advance(double frame_seconds);

    // True if a full tick is pending; consumes it.
    bool consume_tick();

    // Fraction [0, 1) of the next tick already elapsed; render interpolation
    // factor between the last two simulation states.
    double alpha() const;

    double tick_seconds() const { return tick_seconds_; }

    // Total ticks consumed since construction.
    std::uint64_t tick_count() const { return tick_count_; }

    static constexpr int kMaxPendingTicks = 8;

private:
    double tick_seconds_;
    double accumulator_ = 0.0;
    std::uint64_t tick_count_ = 0;
};

}  // namespace eng
