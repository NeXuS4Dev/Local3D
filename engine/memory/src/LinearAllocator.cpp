#include "local3d/memory/LinearAllocator.hpp"

namespace l3d {

LinearAllocator::LinearAllocator(usize capacityBytes, const char* name)
    : capacity_(capacityBytes), name_(name),
      owned_(std::make_unique<unsigned char[]>(capacityBytes)) {
    base_ = owned_.get();
}

LinearAllocator::LinearAllocator(void* memory, usize capacityBytes, const char* name)
    : base_(static_cast<unsigned char*>(memory)), capacity_(capacityBytes), name_(name) {}

LinearAllocator::~LinearAllocator() = default;

void* LinearAllocator::Allocate(usize size, usize alignment, AllocationTag /*tag*/) {
    L3D_ASSERT(IsPowerOfTwo(alignment));
    const usize aligned = AlignUp(offset_, alignment);
    if (aligned + size > capacity_) {
        return nullptr; // Out of memory: the caller decides what to do.
    }
    offset_ = aligned + size;
    if (offset_ > peak_) {
        peak_ = offset_;
    }
    ++allocationCount_;
    return base_ + aligned;
}

void LinearAllocator::Deallocate(void* /*ptr*/, usize /*size*/, usize /*alignment*/) {
    // Intentionally a no-op: linear memory is reclaimed by Reset().
}

MemoryStats LinearAllocator::Snapshot() const noexcept {
    MemoryStats stats;
    stats.allocatedBytes = offset_;
    stats.peakBytes = peak_;
    stats.allocationCount = offset_ > 0 ? 1 : 0;
    stats.totalAllocations = allocationCount_;
    stats.totalDeallocations = 0;
    return stats;
}

FrameAllocator::FrameAllocator(usize capacityPerFrame, u32 frameCount, const char* name)
    : capacityPerFrame_(capacityPerFrame), frameCount_(frameCount > 0 ? frameCount : 1) {
    buffers_.reserve(frameCount_);
    for (u32 i = 0; i < frameCount_; ++i) {
        buffers_.push_back(std::make_unique<LinearAllocator>(capacityPerFrame_, name));
    }
}

LinearAllocator& FrameAllocator::Current() noexcept { return *buffers_[current_]; }

void FrameAllocator::BeginFrame() {
    current_ = (current_ + 1) % frameCount_;
    buffers_[current_]->Reset();
}

} // namespace l3d
