#include "local3d/memory/PoolAllocator.hpp"

namespace l3d {
namespace {

[[nodiscard]] usize EffectiveBlockSize(usize blockSize) {
    // A block must hold at least a free-list pointer.
    const usize minimum = sizeof(void*) < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                                                    : sizeof(void*);
    const usize effective = blockSize < minimum ? minimum : blockSize;
    return AlignUp(effective, alignof(std::max_align_t));
}

} // namespace

PoolAllocator::PoolAllocator(usize blockSize, usize blockCount, const char* name)
    : blockSize_(EffectiveBlockSize(blockSize)), blockCount_(blockCount), name_(name),
      owned_(std::make_unique<unsigned char[]>(EffectiveBlockSize(blockSize) * blockCount)) {
    base_ = owned_.get();
    freeList_ = nullptr;
    for (usize i = 0; i < blockCount_; ++i) {
        auto* block = reinterpret_cast<FreeBlock*>(base_ + i * blockSize_); // NOLINT
        block->next = freeList_;
        freeList_ = block;
    }
    freeCount_ = blockCount_;
}

PoolAllocator::~PoolAllocator() = default;

void* PoolAllocator::Allocate(usize size, usize alignment, AllocationTag /*tag*/) {
    if (size > blockSize_ || freeList_ == nullptr) {
        return nullptr;
    }
    if (alignment > alignof(std::max_align_t)) {
        return nullptr; // Over-aligned types do not belong in a fixed pool.
    }
    FreeBlock* block = freeList_;
    freeList_ = block->next;
    --freeCount_;
    const usize used = blockCount_ - freeCount_;
    if (used > peakUsed_) {
        peakUsed_ = used;
    }
    ++totalAllocations_;
    return block;
}

void PoolAllocator::Deallocate(void* ptr, usize /*size*/, usize /*alignment*/) {
    if (ptr == nullptr) {
        return;
    }
    L3D_ASSERT_MSG(Owns(ptr), "PoolAllocator: freeing memory that does not belong to this pool");
    auto* block = static_cast<FreeBlock*>(ptr);
    block->next = freeList_;
    freeList_ = block;
    ++freeCount_;
}

MemoryStats PoolAllocator::Snapshot() const noexcept {
    MemoryStats stats;
    stats.allocatedBytes = (blockCount_ - freeCount_) * blockSize_;
    stats.peakBytes = peakUsed_ * blockSize_;
    stats.allocationCount = blockCount_ - freeCount_;
    stats.totalAllocations = totalAllocations_;
    stats.totalDeallocations = totalAllocations_ - (blockCount_ - freeCount_);
    return stats;
}

bool PoolAllocator::Owns(const void* ptr) const noexcept {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    if (bytes < base_ || bytes >= base_ + blockSize_ * blockCount_) {
        return false;
    }
    return (static_cast<usize>(bytes - base_) % blockSize_) == 0;
}

} // namespace l3d
