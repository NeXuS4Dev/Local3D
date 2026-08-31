#pragma once
/// @file Containers.hpp
/// @brief Shared container aliases plus small fixed capacity helpers.

#include "local3d/core/Hash.hpp"

#include <atomic>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace l3d {

/// Engine wide string map.  Uses the engine hash so iteration and lookups do
/// not depend on libstdc++ internals (and so profiling is comparable across
/// standard library versions).
template <typename T>
using StringMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

template <typename T>
using HashSet = std::unordered_set<T>;

/// Fixed capacity bitset without std::bitset's awkward word interface.
template <usize Bits>
class BitSet {
    static_assert(Bits > 0, "BitSet needs at least one bit");

public:
    void Set(usize index, bool value = true) noexcept {
        if (index >= Bits) {
            return;
        }
        const usize word = index / 64;
        const u64 mask = 1ULL << (index % 64);
        if (value) {
            words_[word] |= mask;
        } else {
            words_[word] &= ~mask;
        }
    }

    [[nodiscard]] bool Test(usize index) const noexcept {
        if (index >= Bits) {
            return false;
        }
        return (words_[index / 64] & (1ULL << (index % 64))) != 0;
    }

    void Clear() noexcept {
        for (u64& word : words_) {
            word = 0;
        }
    }

    [[nodiscard]] usize Count() const noexcept {
        usize count = 0;
        for (const u64 word : words_) {
            count += static_cast<usize>(__builtin_popcountll(word));
        }
        return count;
    }

    [[nodiscard]] bool Any() const noexcept {
        for (const u64 word : words_) {
            if (word != 0) {
                return true;
            }
        }
        return false;
    }

    friend bool operator==(const BitSet& a, const BitSet& b) noexcept {
        for (usize i = 0; i < kWords; ++i) {
            if (a.words_[i] != b.words_[i]) {
                return false;
            }
        }
        return true;
    }

private:
    static constexpr usize kWords = (Bits + 63) / 64;
    u64 words_[kWords]{};
};

/// Single producer / single consumer ring buffer.  Lock free, no allocation
/// after construction.  Used for handing audio frames and input events between
/// exactly two threads; the job system uses the MPMC queue in Core instead.
template <typename T, usize Capacity>
class SpscRingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    [[nodiscard]] bool Push(const T& value) noexcept {
        const usize write = writeIndex_.load(std::memory_order_relaxed);
        const usize next = (write + 1) & (Capacity - 1);
        if (next == readIndex_.load(std::memory_order_acquire)) {
            return false; // Full.
        }
        slots_[write] = value;
        writeIndex_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool Pop(T& out) noexcept {
        const usize read = readIndex_.load(std::memory_order_relaxed);
        if (read == writeIndex_.load(std::memory_order_acquire)) {
            return false; // Empty.
        }
        out = slots_[read];
        readIndex_.store((read + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return readIndex_.load(std::memory_order_acquire) == writeIndex_.load(std::memory_order_acquire);
    }

    [[nodiscard]] usize CapacityLimit() const noexcept { return Capacity - 1; }

private:
    T slots_[Capacity]{};
    alignas(64) std::atomic<usize> readIndex_{0};
    alignas(64) std::atomic<usize> writeIndex_{0};
};

} // namespace l3d
