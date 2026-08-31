#include "local3d/core/c/l3d_allocator.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if !defined(_WIN32)
#    include <cstring>
#endif

// C ABI allocators.  The engine's own allocators live in Local3D::Memory (C++);
// these expose a minimal, ABI stable subset for plugins and C code.

namespace {

void* SystemAllocate(l3d_allocator* /*self*/, size_t size, size_t alignment, const char* /*tag*/) {
    if (size == 0) {
        return nullptr;
    }
    void* ptr = nullptr;
    if (alignment <= alignof(std::max_align_t)) {
        ptr = std::malloc(size);
    } else {
#if defined(_WIN32)
        ptr = _aligned_malloc(size, alignment);
#else
        if (posix_memalign(&ptr, alignment, size) != 0) {
            ptr = nullptr;
        }
#endif
    }
    return ptr;
}

void SystemDeallocate(l3d_allocator* /*self*/, void* ptr, size_t /*size*/, size_t alignment) {
    if (ptr == nullptr) {
        return;
    }
#if defined(_WIN32)
    if (alignment > alignof(std::max_align_t)) {
        _aligned_free(ptr);
        return;
    }
#else
    (void)alignment;
#endif
    std::free(ptr);
}

l3d_allocator gSystemAllocator = {&SystemAllocate, &SystemDeallocate, nullptr, nullptr,
                                  L3D_ALLOCATOR_ABI_VERSION};

/// Bump allocator carved out of a caller owned block.  Deliberately trivial:
/// no free list, reset only.  Alignment is honoured by padding the offset.
struct LinearHeader {
    unsigned char* base = nullptr;
    size_t capacity = 0;
    size_t offset = 0;
    l3d_allocator allocator{};
};

void* LinearAllocate(l3d_allocator* self, size_t size, size_t alignment, const char* /*tag*/) {
    auto* header = static_cast<LinearHeader*>(self->user_data);
    const size_t mask = alignment > 0 ? alignment - 1 : 0;
    const size_t aligned = (header->offset + mask) & ~mask;
    if (aligned + size > header->capacity) {
        return nullptr;
    }
    header->offset = aligned + size;
    return header->base + aligned;
}

void LinearDeallocate(l3d_allocator* /*self*/, void* /*ptr*/, size_t /*size*/,
                      size_t /*alignment*/) {
    // Linear allocators only support bulk reset.
}

void LinearStats(const l3d_allocator* self, uint64_t* bytes_in_use, uint64_t* allocation_count) {
    const auto* header = static_cast<const LinearHeader*>(self->user_data);
    if (bytes_in_use != nullptr) {
        *bytes_in_use = static_cast<uint64_t>(header->offset);
    }
    if (allocation_count != nullptr) {
        *allocation_count = 0; // Not tracked; keep the contract honest.
    }
}

} // namespace

extern "C" {

l3d_allocator* l3d_allocator_system(void) { return &gSystemAllocator; }

l3d_allocator* l3d_allocator_linear_create(void* memory, size_t capacity) {
    if (memory == nullptr || capacity < sizeof(LinearHeader) + 64) {
        return nullptr;
    }
    auto* header = static_cast<LinearHeader*>(memory);
    header->base = static_cast<unsigned char*>(memory) + sizeof(LinearHeader);
    header->capacity = capacity - sizeof(LinearHeader);
    header->offset = 0;
    header->allocator.allocate = &LinearAllocate;
    header->allocator.deallocate = &LinearDeallocate;
    header->allocator.stats = &LinearStats;
    header->allocator.user_data = header;
    header->allocator.abi_version = L3D_ALLOCATOR_ABI_VERSION;
    return &header->allocator;
}

void l3d_allocator_linear_reset(l3d_allocator* allocator) {
    if (allocator == nullptr || allocator->allocate != &LinearAllocate) {
        return;
    }
    auto* header = static_cast<LinearHeader*>(allocator->user_data);
    header->offset = 0;
}

void l3d_allocator_destroy(l3d_allocator* allocator) {
    // The linear allocator lives inside caller owned memory, so there is
    // nothing to release.  The call exists to keep ownership explicit.
    (void)allocator;
}

} // extern "C"
