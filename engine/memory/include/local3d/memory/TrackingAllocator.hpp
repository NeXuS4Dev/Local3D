#pragma once
/// @file TrackingAllocator.hpp
/// @brief Allocator decorator that records every live allocation.
///
/// Used in Debug/Development builds and by tests to catch leaks, double frees
/// and mismatched sizes.  Costs one header per allocation plus a mutex, so it
/// is deliberately *not* used in shipping builds.

#include "local3d/memory/Allocator.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace l3d {

class TrackingAllocator final : public IAllocator {
public:
    explicit TrackingAllocator(IAllocator& upstream, const char* name = "tracking");
    ~TrackingAllocator() override;

    void* Allocate(usize size, usize alignment, AllocationTag tag) override;
    void Deallocate(void* ptr, usize size, usize alignment) override;
    [[nodiscard]] const char* Name() const noexcept override { return name_; }
    [[nodiscard]] MemoryStats Snapshot() const noexcept override;

    /// Live allocations grouped by tag, sorted by bytes descending.
    struct TagReport {
        std::string tag;
        u64 bytes = 0;
        u64 count = 0;
    };
    [[nodiscard]] std::vector<TagReport> ReportByTag() const;

    /// True when nothing is live.  Tests assert this at teardown.
    [[nodiscard]] bool HasLeaks() const noexcept;

    /// Number of detected errors (double free, wrong size, foreign pointer).
    [[nodiscard]] u64 ErrorCount() const noexcept;

private:
    struct Record {
        usize size = 0;
        usize alignment = 0;
        const char* tag = "";
    };

    IAllocator& upstream_;
    mutable std::mutex mutex_;
    std::unordered_map<void*, Record> live_;
    MemoryStats stats_{};
    u64 errors_ = 0;
    const char* name_;
};

} // namespace l3d
