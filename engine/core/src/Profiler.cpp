#include "local3d/core/Profiler.hpp"

#include "local3d/core/Assert.hpp"
#include "local3d/core/ThreadId.hpp"
#include "local3d/core/Time.hpp"

namespace l3d::prof {
namespace {
constexpr usize kMaxMarkerDepth = 32;
}

struct Profiler::ThreadState {
    u32 threadId = 0;
    u32 depth = 0;
    std::vector<Marker> markers;
};

Profiler& Profiler::Instance() noexcept {
    static Profiler instance;
    return instance;
}

Profiler::~Profiler() {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.clear();
    counters_.clear();
}

Profiler::ThreadState& Profiler::GetThreadState() noexcept {
    // One allocation per thread, ever.  The map itself is protected because
    // threads can be created and destroyed at any time.
    static thread_local ThreadState* cached = nullptr;
    if (cached != nullptr) {
        return *cached;
    }
    auto state = std::make_unique<ThreadState>();
    state->threadId = CurrentThreadId();
    state->markers.reserve(512);
    ThreadState* raw = state.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads_.push_back(std::move(state));
    }
    cached = raw;
    return *cached;
}

void Profiler::BeginFrame() noexcept {
    frameIndex_.fetch_add(1, std::memory_order_relaxed);
}

void Profiler::EndFrame() {
    summary_.frameIndex = frameIndex_.load(std::memory_order_relaxed);
    CollectMarkers();
}

void Profiler::PushMarker(const char* name) noexcept {
    ThreadState& state = GetThreadState();
    if (state.depth >= kMaxMarkerDepth) {
        // Silently ignore over-deep nesting rather than corrupting the stack.
        ++state.depth;
        return;
    }
    Marker marker;
    marker.name = name;
    marker.startNs = Clock::NowNs();
    marker.depth = state.depth;
    marker.threadId = state.threadId;
    state.markers.push_back(marker);
    ++state.depth;
}

void Profiler::PopMarker() noexcept {
    ThreadState& state = GetThreadState();
    if (state.depth == 0) {
        return;
    }
    --state.depth;
    if (state.depth < kMaxMarkerDepth && !state.markers.empty()) {
        state.markers.back().endNs = Clock::NowNs();
    }
}

std::vector<Marker> Profiler::CollectMarkers() {
    std::vector<Marker> collected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        usize total = 0;
        for (const auto& state : threads_) {
            total += state->markers.size();
        }
        collected.reserve(total);
        for (const auto& state : threads_) {
            collected.insert(collected.end(), state->markers.begin(), state->markers.end());
            state->markers.clear();
            state->depth = 0;
        }
        lastFrameMarkers_ = collected;
    }
    summary_.markerCount = collected.size();
    return collected;
}

f64 Profiler::TotalTimeMs(std::string_view name) const {
    f64 totalNs = 0.0;
    for (const Marker& marker : lastFrameMarkers_) {
        if (name == std::string_view(marker.name)) {
            totalNs += static_cast<f64>(marker.endNs - marker.startNs);
        }
    }
    return totalNs / 1.0e6;
}

void Profiler::IncrementCounter(std::string_view name, u64 amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& counter : counters_) {
        if (counter->name == name) {
            counter->value.fetch_add(amount, std::memory_order_relaxed);
            return;
        }
    }
    auto counter = std::make_unique<Counter>();
    counter->name = std::string(name);
    counter->value.store(amount, std::memory_order_relaxed);
    counters_.push_back(std::move(counter));
}

void Profiler::SetCounter(std::string_view name, u64 value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& counter : counters_) {
        if (counter->name == name) {
            counter->value.store(value, std::memory_order_relaxed);
            return;
        }
    }
    auto counter = std::make_unique<Counter>();
    counter->name = std::string(name);
    counter->value.store(value, std::memory_order_relaxed);
    counters_.push_back(std::move(counter));
}

u64 Profiler::GetCounter(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& counter : counters_) {
        if (counter->name == name) {
            return counter->value.load(std::memory_order_relaxed);
        }
    }
    return 0;
}

std::vector<std::pair<std::string, u64>> Profiler::SnapshotCounters() const {
    std::vector<std::pair<std::string, u64>> snapshot;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.reserve(counters_.size());
    for (const auto& counter : counters_) {
        snapshot.emplace_back(counter->name, counter->value.load(std::memory_order_relaxed));
    }
    return snapshot;
}

} // namespace l3d::prof
