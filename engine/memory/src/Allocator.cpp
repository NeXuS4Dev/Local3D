#include "local3d/memory/Allocator.hpp"

#include <cstdlib>

namespace l3d {
namespace {

/// malloc backed allocator.  Alignment above the natural maximum uses
/// aligned_alloc / _aligned_malloc.
class MallocAllocator final : public IAllocator {
public:
    void* Allocate(usize size, usize alignment, AllocationTag /*tag*/) override {
        if (size == 0) {
            return nullptr;
        }
        if (alignment <= alignof(std::max_align_t)) {
            return std::malloc(size);
        }
#if defined(_WIN32)
        return _aligned_malloc(size, alignment);
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0) {
            return nullptr;
        }
        return ptr;
#endif
    }

    void Deallocate(void* ptr, usize /*size*/, usize alignment) override {
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

    [[nodiscard]] const char* Name() const noexcept override { return "malloc"; }
};

MallocAllocator gMallocAllocator;

} // namespace

IAllocator& DefaultAllocator() noexcept { return gMallocAllocator; }

} // namespace l3d
