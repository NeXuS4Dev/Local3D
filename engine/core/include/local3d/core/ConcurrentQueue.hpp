#pragma once
/// @file ConcurrentQueue.hpp
/// @brief Bounded, lock free, multi-producer/multi-consumer FIFO queue.
///
/// Implementation: Dmitry Vyukov's bounded MPMC queue.  Each cell carries a
/// sequence number which encodes its state, so producers and consumers never
/// need a mutex and never block each other - a full queue makes TryEnqueue
/// return false, which callers turn into backpressure.
///
/// Capacity must be a power of two; the constructor rounds up.
///
/// Thread safety: any number of threads may enqueue and dequeue concurrently.

#include "local3d/core/Common.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace l3d {

template <typename T>
class ConcurrentQueue {
public:
    explicit ConcurrentQueue(u32 requestedCapacity)
        : mask_(RoundUpToPowerOfTwo(requestedCapacity) - 1),
          cells_(std::make_unique<Cell[]>(RoundUpToPowerOfTwo(requestedCapacity))) {
        const u32 capacity = mask_ + 1;
        for (u32 i = 0; i < capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ConcurrentQueue(const ConcurrentQueue&) = delete;
    ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;

    /// Non-blocking enqueue.  Returns false when the queue is full; on failure
    /// `value` is left untouched so the caller can still use it (this is what
    /// lets the job system fall back to running a job inline).
    template <typename U>
    [[nodiscard]] bool TryEnqueue(U&& value) {
        u64 pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[static_cast<usize>(pos & mask_)];
            const u64 sequence = cell.sequence.load(std::memory_order_acquire);
            const i64 diff = static_cast<i64>(sequence) - static_cast<i64>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.value.emplace(std::forward<U>(value));
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // Full.
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Non-blocking dequeue.  Returns false when the queue is empty.
    [[nodiscard]] bool TryDequeue(T& out) {
        u64 pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[static_cast<usize>(pos & mask_)];
            const u64 sequence = cell.sequence.load(std::memory_order_acquire);
            const i64 diff = static_cast<i64>(sequence) - static_cast<i64>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = std::move(*cell.value);
                    cell.value.reset();
                    cell.sequence.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // Empty.
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Approximate number of queued items (racy by nature; for diagnostics).
    [[nodiscard]] u32 SizeApprox() const noexcept {
        const u64 tail = tail_.load(std::memory_order_relaxed);
        const u64 head = head_.load(std::memory_order_relaxed);
        return static_cast<u32>(tail >= head ? tail - head : 0);
    }

    [[nodiscard]] u32 Capacity() const noexcept { return mask_ + 1; }

    [[nodiscard]] static constexpr u32 RoundUpToPowerOfTwo(u32 value) noexcept {
        u32 result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

private:
    struct Cell {
        std::atomic<u64> sequence{0};
        std::optional<T> value;
    };

    const u32 mask_;
    std::unique_ptr<Cell[]> cells_;
    alignas(64) std::atomic<u64> head_{0};
    alignas(64) std::atomic<u64> tail_{0};
};

} // namespace l3d
