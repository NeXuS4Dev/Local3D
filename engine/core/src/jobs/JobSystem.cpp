#include "local3d/core/jobs/JobSystem.hpp"

#include "local3d/core/Assert.hpp"
#include "local3d/core/ConcurrentQueue.hpp"
#include "local3d/core/Log.hpp"
#include "local3d/core/ThreadId.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace l3d::jobs {
namespace {

/// One unit of work in the queue.  The state is shared with the handle so that
/// the job stays valid even if the caller drops its handle early.
struct QueuedJob {
    JobFunction function;
    std::shared_ptr<JobState> state;
};

[[nodiscard]] u32 DetectWorkerCount(u32 requested) noexcept {
    if (requested > 0) {
        return requested;
    }
    const u32 hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? hardware - 1 : 1;
}

} // namespace

struct JobSystem::Impl {
    explicit Impl(u32 queueCapacity) : queue(queueCapacity) {}

    ConcurrentQueue<QueuedJob> queue;

    std::mutex wakeMutex;
    std::condition_variable wakeCv;
    std::atomic<bool> stopping{false};
    std::atomic<u32> idleWorkers{0};

    std::atomic<u64> submitted{0};
    std::atomic<u64> executed{0};
    std::atomic<u64> executedInline{0};
    std::atomic<u64> queueFullEvents{0};

    std::mutex waitMutex;
    std::condition_variable waitCv;
    std::atomic<u64> outstanding{0};

    std::vector<std::thread> workers;

    static thread_local u32 tlsWorkerIndex;
};

thread_local u32 JobSystem::Impl::tlsWorkerIndex = 0;

JobSystem::JobSystem(const Desc& desc)
    : impl_(std::make_unique<Impl>(desc.queueCapacity)), workerCount_(DetectWorkerCount(desc.workerCount)),
      queueCapacity_(ConcurrentQueue<QueuedJob>::RoundUpToPowerOfTwo(desc.queueCapacity)) {}

std::unique_ptr<JobSystem> JobSystem::Create(const Desc& desc) {
    std::unique_ptr<JobSystem> system(new JobSystem(desc));
    if (!system->StartWorkers()) {
        return nullptr;
    }
    return system;
}

JobSystem::~JobSystem() { Shutdown(); }

bool JobSystem::StartWorkers() {
    impl_->workers.reserve(workerCount_);
    try {
        for (u32 i = 0; i < workerCount_; ++i) {
            impl_->workers.emplace_back([this, i] { WorkerLoop(i); });
        }
    } catch (const std::system_error& error) {
        L3D_LOG_ERROR(LogCategory::Jobs, "JobSystem: failed to start workers: {}", error.what());
        Shutdown();
        return false;
    }
    return true;
}

void JobSystem::Shutdown() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopping.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->wakeMutex);
    }
    impl_->wakeCv.notify_all();
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    impl_->workers.clear();

    // Drain anything left so shared states complete and handles never hang.
    QueuedJob job;
    while (impl_->queue.TryDequeue(job)) {
        if (job.function) {
            job.function();
        }
        if (job.state) {
            job.state->Arrive();
        }
        impl_->outstanding.fetch_sub(1, std::memory_order_acq_rel);
    }
    impl_->waitCv.notify_all();
}

JobHandle JobSystem::Submit(JobFunction fn) {
    if (!fn) {
        return JobHandle{};
    }
    auto state = std::make_shared<JobState>(1);
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    impl_->outstanding.fetch_add(1, std::memory_order_acq_rel);

    QueuedJob job{std::move(fn), state};
    if (!impl_->queue.TryEnqueue(std::move(job))) {
        // Backpressure: run it here instead of growing the queue without bound.
        impl_->queueFullEvents.fetch_add(1, std::memory_order_relaxed);
        impl_->executedInline.fetch_add(1, std::memory_order_relaxed);
        // TryEnqueue leaves the job intact when it fails, so it is still ours.
        if (job.function) {
            job.function();
        }
        state->Arrive();
        if (impl_->outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            impl_->waitCv.notify_all();
        }
        return JobHandle{std::move(state)};
    }

    {
        std::lock_guard<std::mutex> lock(impl_->wakeMutex);
    }
    impl_->wakeCv.notify_one();
    return JobHandle{std::move(state)};
}

JobHandle JobSystem::Submit(std::vector<JobFunction> jobs) {
    if (jobs.empty()) {
        return JobHandle{};
    }
    auto state = std::make_shared<JobState>(static_cast<u32>(jobs.size()));
    for (auto& fn : jobs) {
        if (!fn) {
            state->Arrive();
            continue;
        }
        impl_->submitted.fetch_add(1, std::memory_order_relaxed);
        impl_->outstanding.fetch_add(1, std::memory_order_acq_rel);
        QueuedJob job{std::move(fn), state};
        if (!impl_->queue.TryEnqueue(std::move(job))) {
            impl_->queueFullEvents.fetch_add(1, std::memory_order_relaxed);
            impl_->executedInline.fetch_add(1, std::memory_order_relaxed);
            if (job.function) {
                job.function();
            }
            state->Arrive();
            if (impl_->outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                impl_->waitCv.notify_all();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->wakeMutex);
    }
    impl_->wakeCv.notify_all();
    return JobHandle{std::move(state)};
}

JobHandle JobSystem::ParallelFor(u32 itemCount, u32 grainSize,
                                 const std::function<void(u32, u32)>& body) {
    if (itemCount == 0 || !body) {
        return JobHandle{};
    }
    const u32 grain = grainSize > 0 ? grainSize : 1;
    const u32 chunkCount = (itemCount + grain - 1) / grain;

    std::vector<JobFunction> jobs;
    jobs.reserve(chunkCount);
    for (u32 chunk = 0; chunk < chunkCount; ++chunk) {
        const u32 begin = chunk * grain;
        const u32 end = begin + grain < itemCount ? begin + grain : itemCount;
        jobs.push_back([&body, begin, end] { body(begin, end); });
    }
    return Submit(std::move(jobs));
}

void JobSystem::WaitAll() {
    std::unique_lock<std::mutex> lock(impl_->waitMutex);
    impl_->waitCv.wait(lock, [this] { return impl_->outstanding.load(std::memory_order_acquire) == 0; });
}

u32 JobSystem::CurrentWorkerIndex() const noexcept { return Impl::tlsWorkerIndex; }

u64 JobSystem::QueueFullEvents() const noexcept {
    return impl_->queueFullEvents.load(std::memory_order_relaxed);
}

JobStats JobSystem::Stats() const noexcept {
    JobStats stats;
    stats.submitted = impl_->submitted.load(std::memory_order_relaxed);
    stats.executed = impl_->executed.load(std::memory_order_relaxed);
    stats.executedInline = impl_->executedInline.load(std::memory_order_relaxed);
    stats.workerCount = workerCount_;
    stats.queueDepth = impl_->queue.SizeApprox();
    return stats;
}

bool JobSystem::TryPop(JobFunction& out) {
    QueuedJob job;
    if (!impl_->queue.TryDequeue(job)) {
        return false;
    }
    out = [fn = std::move(job.function), state = std::move(job.state), this] {
        if (fn) {
            fn();
        }
        impl_->executed.fetch_add(1, std::memory_order_relaxed);
        if (state) {
            state->Arrive();
        }
        if (impl_->outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            impl_->waitCv.notify_all();
        }
    };
    return true;
}

void JobSystem::WorkerLoop(u32 workerIndex) {
    Impl::tlsWorkerIndex = workerIndex + 1;

    while (!impl_->stopping.load(std::memory_order_acquire)) {
        JobFunction job;
        if (TryPop(job)) {
            job();
            continue;
        }
        impl_->idleWorkers.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(impl_->wakeMutex);
            impl_->wakeCv.wait_for(lock, std::chrono::milliseconds(2), [this] {
                return impl_->stopping.load(std::memory_order_acquire) ||
                       impl_->queue.SizeApprox() > 0;
            });
        }
        impl_->idleWorkers.fetch_sub(1, std::memory_order_relaxed);
    }

    // Finish the queue on the way out so no handle is left dangling.
    JobFunction job;
    while (TryPop(job)) {
        job();
    }
    Impl::tlsWorkerIndex = 0;
}

void ParallelForBlocking(JobSystem& jobs, u32 itemCount, u32 grainSize,
                         const std::function<void(u32, u32)>& body) {
    jobs.ParallelFor(itemCount, grainSize, body).Wait();
}

} // namespace l3d::jobs
