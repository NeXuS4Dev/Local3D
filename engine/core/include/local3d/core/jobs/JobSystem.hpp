#pragma once
/// @file jobs/JobSystem.hpp
/// @brief A small, deterministic-friendly job scheduler.
///
/// Model
/// -----
///  * One shared, bounded, lock free MPMC queue feeds a fixed pool of workers.
///  * Jobs are `std::function<void()>` so closures can capture freely.
///  * `JobHandle` is a shared counter: waiting blocks on a condition variable
///    until every job of the handle has retired.
///  * `ParallelFor` splits a range into chunks and returns one handle covering
///    all of them.
///
/// Backpressure: when the queue is full the submitting thread executes the job
/// inline.  This keeps the scheduler deadlock free without unbounded memory.
///
/// Threading assumptions
/// ---------------------
///  * Submit/Wait are thread safe from any thread (including workers), except
///    that a worker must never wait on a handle that can only complete by
///    running on that same worker - the classic job system cycle.
///  * Job bodies must be re-entrant and must not assume a particular thread.
///
/// Why not work stealing?  A shared MPMC queue is far easier to reason about
/// and to test, and at engine scale (a handful of workers) contention is not
/// the bottleneck.  Per-thread deques are a documented roadmap item
/// (docs/architecture/job-system.md).

#include "local3d/core/Common.hpp"
#include "local3d/core/Status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::jobs {

/// Shared completion state behind a JobHandle.
class JobState {
public:
    explicit JobState(u32 pending) noexcept : pending_(pending) {}

    void Arrive() noexcept {
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            cv_.notify_all();
        }
    }

    [[nodiscard]] bool IsComplete() const noexcept {
        return pending_.load(std::memory_order_acquire) == 0;
    }

    [[nodiscard]] u32 Pending() const noexcept { return pending_.load(std::memory_order_acquire); }

    void Wait() const {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return IsComplete(); });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool WaitFor(const std::chrono::duration<Rep, Period>& timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return IsComplete(); });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<u32> pending_;
};

/// A reference to one or more in flight jobs.
class JobHandle {
public:
    JobHandle() noexcept = default;
    explicit JobHandle(std::shared_ptr<JobState> state) noexcept : state_(std::move(state)) {}

    [[nodiscard]] bool IsValid() const noexcept { return static_cast<bool>(state_); }
    [[nodiscard]] bool IsComplete() const noexcept {
        return !state_ || state_->IsComplete();
    }
    void Wait() const {
        if (state_) {
            state_->Wait();
        }
    }
    template <typename Rep, typename Period>
    [[nodiscard]] bool WaitFor(const std::chrono::duration<Rep, Period>& timeout) const {
        return !state_ || state_->WaitFor(timeout);
    }

private:
    std::shared_ptr<JobState> state_;
};

using JobFunction = std::function<void()>;

struct JobStats {
    u64 submitted = 0;
    u64 executed = 0;
    u64 executedInline = 0;   ///< Ran on the submitting thread due to a full queue.
    u64 steals = 0;
    u64 waits = 0;
    u32 workerCount = 0;
    u32 queueDepth = 0;
};

class JobSystem {
public:
    struct Desc {
        /// 0 means "hardware concurrency - 1", clamped to at least one worker.
        u32 workerCount = 0;
        u32 queueCapacity = 16384;
        std::string_view threadNamePrefix = "l3d-job";
    };

    /// Create and start the pool.  Returns null on failure (thread creation).
    [[nodiscard]] static std::unique_ptr<JobSystem> Create(const Desc& desc);
    [[nodiscard]] static std::unique_ptr<JobSystem> Create() { return Create(Desc{}); }

    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Queue a job.  Returns a handle that completes when it has run; the
    /// handle may be dropped for fire-and-forget work (see WaitAll).
    JobHandle Submit(JobFunction fn);

    /// Queue several jobs that share a single handle.
    JobHandle Submit(std::vector<JobFunction> jobs);

    /// Split [0, itemCount) into chunks and process them in parallel.
    /// `body` is invoked with a half open range [begin, end).
    JobHandle ParallelFor(u32 itemCount, u32 grainSize,
                                        const std::function<void(u32, u32)>& body);

    /// Block until every submitted job has finished.
    void WaitAll();

    /// Shut the workers down cleanly (also happens in the destructor).
    void Shutdown();

    [[nodiscard]] u32 WorkerCount() const noexcept { return workerCount_; }
    [[nodiscard]] JobStats Stats() const noexcept;

    /// Identifier of the calling thread: 0 == main thread, 1..N == worker.
    [[nodiscard]] u32 CurrentWorkerIndex() const noexcept;
    [[nodiscard]] bool IsWorkerThread() const noexcept { return CurrentWorkerIndex() != 0; }

    /// How often the queue was full and a job had to run on the caller thread.
    [[nodiscard]] u64 QueueFullEvents() const noexcept;

private:
    explicit JobSystem(const Desc& desc);
    bool StartWorkers();
    void WorkerLoop(u32 workerIndex);
    bool TryPop(JobFunction& out);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    u32 workerCount_ = 0;
    u32 queueCapacity_ = 0;
};

/// Convenience: run a parallel for over a range and block until it is done.
void ParallelForBlocking(JobSystem& jobs, u32 itemCount, u32 grainSize,
                         const std::function<void(u32, u32)>& body);

} // namespace l3d::jobs
