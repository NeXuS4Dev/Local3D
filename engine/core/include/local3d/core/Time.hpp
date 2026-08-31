#pragma once
/// @file Time.hpp
/// @brief Monotonic clocks, frame timing and a fixed timestep accumulator.

#include "local3d/core/Common.hpp"

#include <chrono>

namespace l3d {

/// A point on the monotonic clock, in nanoseconds since engine start.
struct Timestamp {
    u64 ns = 0;

    [[nodiscard]] constexpr u64 Millis() const noexcept { return ns / 1'000'000ULL; }
    [[nodiscard]] constexpr f64 Seconds() const noexcept { return static_cast<f64>(ns) / 1e9; }
    friend constexpr bool operator==(Timestamp a, Timestamp b) noexcept { return a.ns == b.ns; }
    friend constexpr bool operator<(Timestamp a, Timestamp b) noexcept { return a.ns < b.ns; }
    friend constexpr Timestamp operator-(Timestamp a, Timestamp b) noexcept {
        return Timestamp{a.ns - b.ns};
    }
    friend constexpr Timestamp operator+(Timestamp a, Timestamp b) noexcept {
        return Timestamp{a.ns + b.ns};
    }
};

/// Duration between two timestamps, expressed in seconds.
struct TimeDelta {
    f32 seconds = 0.0f;

    [[nodiscard]] constexpr f32 Millis() const noexcept { return seconds * 1000.0f; }
    [[nodiscard]] static constexpr TimeDelta FromSeconds(f32 s) noexcept { return TimeDelta{s}; }
    [[nodiscard]] static constexpr TimeDelta FromMillis(f32 ms) noexcept {
        return TimeDelta{ms / 1000.0f};
    }
};

class Clock {
public:
    /// Nanoseconds since an unspecified epoch; only differences are meaningful.
    [[nodiscard]] static u64 NowNs() noexcept;
    [[nodiscard]] static Timestamp Now() noexcept { return Timestamp{NowNs()}; }
};

/// Measures elapsed time between Start() and Elapsed() calls.
class Stopwatch {
public:
    Stopwatch() noexcept { Start(); }
    void Start() noexcept { startNs_ = Clock::NowNs(); }
    [[nodiscard]] u64 ElapsedNs() const noexcept { return Clock::NowNs() - startNs_; }
    [[nodiscard]] f64 ElapsedSeconds() const noexcept {
        return static_cast<f64>(ElapsedNs()) / 1e9;
    }

private:
    u64 startNs_ = 0;
};

/// Fixed timestep accumulator - the standard way to run physics at a constant
/// rate while rendering at whatever rate the display allows.
///
/// Thread safety: not thread safe; drive it from the single thread that owns
/// the frame loop.
class FixedTimestep {
public:
    explicit FixedTimestep(f32 stepSeconds = 1.0f / 60.0f, u32 maxStepsPerFrame = 5) noexcept
        : stepSeconds_(stepSeconds), maxStepsPerFrame_(maxStepsPerFrame) {}

    /// Feed the frame delta; returns how many fixed steps should run.
    [[nodiscard]] u32 Accumulate(f32 frameSeconds) noexcept {
        accumulator_ += frameSeconds;
        u32 steps = 0;
        while (accumulator_ >= stepSeconds_ && steps < maxStepsPerFrame_) {
            accumulator_ -= stepSeconds_;
            ++steps;
        }
        if (steps == maxStepsPerFrame_) {
            // Drop the backlog rather than spiralling (the "death spiral").
            accumulator_ = 0.0f;
        }
        return steps;
    }

    /// Interpolation alpha in [0,1) for rendering between two physics states.
    [[nodiscard]] f32 Alpha() const noexcept { return accumulator_ / stepSeconds_; }

    [[nodiscard]] f32 StepSeconds() const noexcept { return stepSeconds_; }
    void Reset() noexcept { accumulator_ = 0.0f; }

private:
    f32 stepSeconds_;
    u32 maxStepsPerFrame_;
    f32 accumulator_ = 0.0f;
};

/// Tracks smoothed frame timing for HUDs and profiling output.
class FrameClock {
public:
    void BeginFrame() noexcept { frameStartNs_ = Clock::NowNs(); }

    /// Call at the end of a frame; updates the smoothed statistics.
    void EndFrame() noexcept {
        const u64 elapsed = Clock::NowNs() - frameStartNs_;
        const f64 seconds = static_cast<f64>(elapsed) / 1e9;
        const f64 smoothing = 0.1;
        smoothedSeconds_ = smoothedSeconds_ * (1.0 - smoothing) + seconds * smoothing;
        ++frameCount_;
    }

    [[nodiscard]] f64 FrameSeconds() const noexcept { return smoothedSeconds_; }
    [[nodiscard]] f64 FramesPerSecond() const noexcept {
        return smoothedSeconds_ > 0.0 ? 1.0 / smoothedSeconds_ : 0.0;
    }
    [[nodiscard]] u64 FrameCount() const noexcept { return frameCount_; }

    /// Force the next EndFrame to report `seconds` (used by tests and by the
    /// headless "virtual time" mode).
    void SetVirtualFrameSeconds(f64 seconds) noexcept { virtualSeconds_ = seconds; }

private:
    u64 frameStartNs_ = 0;
    f64 smoothedSeconds_ = 1.0 / 60.0;
    u64 frameCount_ = 0;
    f64 virtualSeconds_ = 0.0;
};

} // namespace l3d
