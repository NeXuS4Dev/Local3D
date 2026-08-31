#pragma once
/// @file LinearAllocator.hpp
/// @brief Bump allocator over a caller owned block.  The workhorse for frame
///        temporaries: allocate freely, reset in bulk, never free individually.

#include "local3d/core/Assert.hpp"
#include "local3d/memory/Allocator.hpp"

#include <memory>
#include <vector>

namespace l3d {

/// Non-thread-safe bump allocator.  Individual deallocation is a no-op.
class LinearAllocator final : public IAllocator {
public:
    /// Allocate the backing block itself (owned by this object).
    explicit LinearAllocator(usize capacityBytes, const char* name = "linear");

    /// Wrap memory owned by the caller.  The caller must keep it alive.
    LinearAllocator(void* memory, usize capacityBytes, const char* name);

    ~LinearAllocator() override;

    void* Allocate(usize size, usize alignment, AllocationTag tag) override;
    void Deallocate(void* ptr, usize size, usize alignment) override;
    [[nodiscard]] const char* Name() const noexcept override { return name_; }
    [[nodiscard]] MemoryStats Snapshot() const noexcept override;

    /// Reset to empty.  Invalidates every pointer handed out.
    void Reset() noexcept { offset_ = 0; }

    [[nodiscard]] usize Offset() const noexcept { return offset_; }
    [[nodiscard]] usize Capacity() const noexcept { return capacity_; }
    [[nodiscard]] usize Remaining() const noexcept { return capacity_ - offset_; }
    [[nodiscard]] usize PeakOffset() const noexcept { return peak_; }

    /// RAII marker: everything allocated after construction is released when
    /// the scope object is destroyed (LIFO only).
    class Scope {
    public:
        explicit Scope(LinearAllocator& allocator) noexcept
            : allocator_(&allocator), marker_(allocator.offset_) {}
        ~Scope() {
            if (allocator_ != nullptr) {
                allocator_->offset_ = marker_;
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        LinearAllocator* allocator_;
        usize marker_;
    };

private:
    unsigned char* base_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
    usize peak_ = 0;
    u64 allocationCount_ = 0;
    const char* name_;
    std::unique_ptr<unsigned char[]> owned_;
};

/// Three rotating linear allocators, one per buffered frame, so frame N+1 can
/// build its data while frame N is still reading frame N-1's.
class FrameAllocator {
public:
    explicit FrameAllocator(usize capacityPerFrame, u32 frameCount = 3, const char* name = "frame");

    /// Allocator for the frame currently being recorded.
    [[nodiscard]] LinearAllocator& Current() noexcept;

    /// Advance to the next frame, resetting the buffer that becomes current.
    void BeginFrame();

    [[nodiscard]] u32 FrameCount() const noexcept { return frameCount_; }
    [[nodiscard]] usize CapacityPerFrame() const noexcept { return capacityPerFrame_; }

private:
    usize capacityPerFrame_;
    u32 frameCount_;
    u32 current_ = 0;
    std::vector<std::unique_ptr<LinearAllocator>> buffers_;
};

} // namespace l3d
