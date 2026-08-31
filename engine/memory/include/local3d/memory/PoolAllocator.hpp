#pragma once
/// @file PoolAllocator.hpp
/// @brief Fixed size block pool with an intrusive free list.
///
/// Best for the classic engine case: thousands of equally sized objects
/// (components, particles, render instances) with chaotic lifetimes and no
/// fragmentation tolerance.

#include "local3d/core/Assert.hpp"
#include "local3d/memory/Allocator.hpp"

#include <memory>
#include <vector>

namespace l3d {

/// Single size class.  Allocation is O(1); deallocation is O(1).
/// Thread safety: none - wrap in a mutex or use one pool per thread.
class PoolAllocator final : public IAllocator {
public:
    PoolAllocator(usize blockSize, usize blockCount, const char* name = "pool");
    ~PoolAllocator() override;

    void* Allocate(usize size, usize alignment, AllocationTag tag) override;
    void Deallocate(void* ptr, usize size, usize alignment) override;
    [[nodiscard]] const char* Name() const noexcept override { return name_; }
    [[nodiscard]] MemoryStats Snapshot() const noexcept override;

    [[nodiscard]] usize BlockSize() const noexcept { return blockSize_; }
    [[nodiscard]] usize BlockCount() const noexcept { return blockCount_; }
    [[nodiscard]] usize FreeBlocks() const noexcept { return freeCount_; }
    [[nodiscard]] bool Owns(const void* ptr) const noexcept;

private:
    union FreeBlock {
        FreeBlock* next;
        alignas(std::max_align_t) unsigned char storage[1];
    };

    usize blockSize_;
    usize blockCount_;
    unsigned char* base_ = nullptr;
    FreeBlock* freeList_ = nullptr;
    usize freeCount_ = 0;
    usize peakUsed_ = 0;
    u64 totalAllocations_ = 0;
    const char* name_;
    std::unique_ptr<unsigned char[]> owned_;
};

} // namespace l3d
