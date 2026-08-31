#pragma once
/// @file Allocator.hpp
/// @brief The allocator interface every Local3D subsystem allocates through.
///
/// Why an interface and not just `new`?  Because the engine needs:
///  * per-subsystem accounting (who leaked, who is growing),
///  * frame/pool allocators in hot paths that never touch the system heap,
///  * deterministic, leak-checked teardown in tests.
///
/// Ownership rules:
///  * An allocator never owns the memory it hands out beyond its own bookkeeping
///    bookkeeping block; freeing with a different allocator is a bug.
///  * Allocators are objects with a lifetime managed by their owner (usually a
///    subsystem or the Engine).  They are not global singletons, except for
///    DefaultAllocator() which exists so containers have a sane fallback.
///
/// Threading: implementations document their own guarantees.  IAllocator itself
/// imposes none.

#include "local3d/core/Common.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

namespace l3d {

/// Snapshot of allocator activity.  Cheap to copy, safe to log.
struct MemoryStats {
    u64 allocatedBytes = 0;   ///< Currently live bytes (excluding headers).
    u64 peakBytes = 0;        ///< High water mark of live bytes.
    u64 allocationCount = 0;  ///< Currently live allocations.
    u64 totalAllocations = 0; ///< Lifetime allocation calls.
    u64 totalDeallocations = 0;
};

/// Tag identifying what an allocation is for; shows up in reports.
struct AllocationTag {
    const char* name = "untagged";
};

inline constexpr AllocationTag kTagDefault{"default"};
inline constexpr AllocationTag kTagFrame{"frame"};
inline constexpr AllocationTag kTagAsset{"asset"};
inline constexpr AllocationTag kTagRender{"render"};

/// Common interface.  Implementations must be explicit about thread safety.
class IAllocator {
public:
    virtual ~IAllocator() = default;

    /// Allocate `size` bytes aligned to `alignment` (a power of two).
    /// Returns nullptr on exhaustion; never throws.
    [[nodiscard]] virtual void* Allocate(usize size, usize alignment, AllocationTag tag) = 0;

    /// Release memory returned by Allocate.  `size` must match the original
    /// request - passing it avoids per-allocation bookkeeping in fast paths.
    virtual void Deallocate(void* ptr, usize size, usize alignment) = 0;

    /// Default implementation: allocate + copy + free.
    virtual void* Reallocate(void* ptr, usize oldSize, usize newSize, usize alignment,
                             AllocationTag tag) {
        void* fresh = Allocate(newSize, alignment, tag);
        if (fresh == nullptr) {
            return nullptr;
        }
        const usize copyCount = oldSize < newSize ? oldSize : newSize;
        if (ptr != nullptr && copyCount > 0) {
            std::memcpy(fresh, ptr, copyCount);
        }
        Deallocate(ptr, oldSize, alignment);
        return fresh;
    }

    [[nodiscard]] virtual const char* Name() const noexcept = 0;
    [[nodiscard]] virtual MemoryStats Snapshot() const noexcept { return {}; }
};

/// Alignment helpers used by every allocator implementation.
[[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}
[[nodiscard]] constexpr usize AlignDown(usize value, usize alignment) noexcept {
    return value & ~(alignment - 1);
}
[[nodiscard]] constexpr bool IsAligned(usize value, usize alignment) noexcept {
    return (value & (alignment - 1)) == 0;
}
[[nodiscard]] constexpr bool IsPowerOfTwo(usize value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Process wide fallback allocator (malloc backed).  Used when a subsystem has
/// no explicit allocator, which keeps the common case ergonomic.
[[nodiscard]] IAllocator& DefaultAllocator() noexcept;

/// STL adapter so `std::vector<T, L3DAllocator<T>>` allocates through an
/// IAllocator.  The allocator instance is referenced, never owned.
template <typename T>
class L3DAllocator {
public:
    using value_type = T;

    explicit L3DAllocator(IAllocator& allocator, AllocationTag tag = kTagDefault) noexcept
        : allocator_(&allocator), tag_(tag) {}

    /// Rebind constructor (required by the standard).
    template <typename U>
    L3DAllocator(const L3DAllocator<U>& other) noexcept
        : allocator_(other.GetAllocator()), tag_(other.GetTag()) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        return static_cast<T*>(
            allocator_->Allocate(count * sizeof(T), alignof(T) < 16 ? 16 : alignof(T), tag_));
    }

    void deallocate(T* ptr, std::size_t count) noexcept {
        allocator_->Deallocate(ptr, count * sizeof(T), alignof(T) < 16 ? 16 : alignof(T));
    }

    [[nodiscard]] IAllocator& GetAllocator() const noexcept { return *allocator_; }
    [[nodiscard]] AllocationTag GetTag() const noexcept { return tag_; }

    friend bool operator==(const L3DAllocator& a, const L3DAllocator& b) noexcept {
        return a.allocator_ == b.allocator_;
    }

private:
    IAllocator* allocator_;
    AllocationTag tag_;
};

/// Owning smart pointer bound to an IAllocator.  Used where a unique_ptr must
/// not fall back to the global operator delete.
template <typename T>
class UniquePtr {
public:
    UniquePtr() noexcept = default;
    UniquePtr(std::nullptr_t) noexcept {} // NOLINT(google-explicit-constructor)

    UniquePtr(IAllocator& allocator, AllocationTag tag = kTagDefault) noexcept
        : allocator_(&allocator), tag_(tag) {}

    /// Construct in place with the given allocator.
    template <typename... Args>
    [[nodiscard]] static UniquePtr Create(IAllocator& allocator, AllocationTag tag, Args&&... args) {
        UniquePtr result(allocator, tag);
        result.pointer_ =
            static_cast<T*>(allocator.Allocate(sizeof(T), alignof(T), tag));
        if (result.pointer_ != nullptr) {
            new (result.pointer_) T(std::forward<Args>(args)...); // NOLINT
        }
        return result;
    }

    ~UniquePtr() { Reset(); }

    UniquePtr(UniquePtr&& other) noexcept
        : pointer_(other.pointer_), allocator_(other.allocator_), tag_(other.tag_) {
        other.pointer_ = nullptr;
    }

    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            Reset();
            pointer_ = other.pointer_;
            allocator_ = other.allocator_;
            tag_ = other.tag_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    void Reset() {
        if (pointer_ != nullptr && allocator_ != nullptr) {
            pointer_->~T();
            allocator_->Deallocate(pointer_, sizeof(T), alignof(T));
            pointer_ = nullptr;
        }
    }

    [[nodiscard]] T* Get() const noexcept { return pointer_; }
    [[nodiscard]] T& operator*() const noexcept { return *pointer_; }
    [[nodiscard]] T* operator->() const noexcept { return pointer_; }
    [[nodiscard]] explicit operator bool() const noexcept { return pointer_ != nullptr; }

private:
    T* pointer_ = nullptr;
    IAllocator* allocator_ = nullptr;
    AllocationTag tag_;
};

} // namespace l3d
