#pragma once
/// @file Profiler.hpp
/// @brief Hierarchical CPU profiling markers and named counters.
///
/// Markers are recorded per thread into a growable buffer and collected at the
/// end of the frame, so the hot path only touches thread local memory.

#include "local3d/core/Common.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::prof {

/// One recorded scope.  Names are static string literals, so we only store the
/// pointer (no copies, no allocation on the hot path).
struct Marker {
    const char* name = "";
    u64 startNs = 0;
    u64 endNs = 0;
    u32 depth = 0;
    u32 threadId = 0;
};

/// A named 64 bit counter (allocation bytes, draw calls, triangles, ...).
struct Counter {
    std::string name;
    std::atomic<u64> value{0};
};

struct FrameSummary {
    u64 frameIndex = 0;
    f64 cpuFrameMs = 0.0;
    usize markerCount = 0;
};

class Profiler {
public:
    [[nodiscard]] static Profiler& Instance() noexcept;

    void SetEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool IsEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    /// Mark the frame boundary.  CollectMarkers() returns the previous frame.
    void BeginFrame() noexcept;
    void EndFrame();

    void PushMarker(const char* name) noexcept;
    void PopMarker() noexcept;

    /// Snapshot (and clear) every marker recorded since the last call.
    std::vector<Marker> CollectMarkers();

    /// Longest total time recorded for a marker name during the last frame (ms).
    [[nodiscard]] f64 TotalTimeMs(std::string_view name) const;

    [[nodiscard]] const FrameSummary& LastFrame() const noexcept { return summary_; }

    /// Named counters.  Counters are process wide and thread safe.
    void IncrementCounter(std::string_view name, u64 amount = 1);
    void SetCounter(std::string_view name, u64 value);
    [[nodiscard]] u64 GetCounter(std::string_view name) const;
    [[nodiscard]] std::vector<std::pair<std::string, u64>> SnapshotCounters() const;

private:
    struct ThreadState;

    Profiler() = default;
    ~Profiler();
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    ThreadState& GetThreadState() noexcept;

    std::atomic<bool> enabled_{true};
    std::atomic<u64> frameIndex_{0};
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ThreadState>> threads_;
    std::vector<Marker> lastFrameMarkers_;
    std::vector<std::unique_ptr<Counter>> counters_;
    FrameSummary summary_{};
};

/// RAII scope marker.
class ScopedMarker {
public:
    explicit ScopedMarker(const char* name) noexcept : name_(name) {
        if (Profiler::Instance().IsEnabled()) {
            Profiler::Instance().PushMarker(name_);
            active_ = true;
        }
    }
    ~ScopedMarker() noexcept {
        if (active_) {
            Profiler::Instance().PopMarker();
        }
    }
    ScopedMarker(const ScopedMarker&) = delete;
    ScopedMarker& operator=(const ScopedMarker&) = delete;

private:
    const char* name_;
    bool active_ = false;
};

} // namespace l3d::prof

#define L3D_PROFILE_SCOPE(name) ::l3d::prof::ScopedMarker L3D_CONCAT(l3d_prof_, __LINE__)(name)
#define L3D_PROFILE_FUNCTION() L3D_PROFILE_SCOPE(__func__)
#define L3D_PROFILE_COUNTER_ADD(name, amount) ::l3d::prof::Profiler::Instance().IncrementCounter(name, amount)
#define L3D_PROFILE_COUNTER_SET(name, value) ::l3d::prof::Profiler::Instance().SetCounter(name, value)
