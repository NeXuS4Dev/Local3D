#ifndef L3D_CORE_C_L3D_ALLOCATOR_H
#define L3D_CORE_C_L3D_ALLOCATOR_H
/* ---------------------------------------------------------------------------
 * Local3D allocator - C ABI.
 *
 * A vtable based allocator interface that C code, plugins and language
 * bindings can implement or consume.  Ownership rules:
 *
 *   - the *owner* of an l3d_allocator keeps it alive for as long as any
 *     consumer holds a pointer to it;
 *   - memory returned by allocate() must be released with deallocate() on the
 *     *same* allocator instance;
 *   - callbacks must not throw (C++ implementations must catch internally).
 * ------------------------------------------------------------------------- */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct l3d_allocator l3d_allocator;

/** Allocate `size` bytes with `alignment` (must be a power of two). */
typedef void* (*l3d_alloc_fn)(l3d_allocator* self, size_t size, size_t alignment,
                              const char* tag);
/** Release memory previously returned by the matching allocate call. */
typedef void (*l3d_dealloc_fn)(l3d_allocator* self, void* ptr, size_t size, size_t alignment);
/** Optional statistics query; may be NULL. */
typedef void (*l3d_stats_fn)(const l3d_allocator* self, uint64_t* bytes_in_use,
                             uint64_t* allocation_count);

struct l3d_allocator {
    l3d_alloc_fn allocate;
    l3d_dealloc_fn deallocate;
    l3d_stats_fn stats; /**< Optional, may be NULL. */
    void* user_data;    /**< Owned by the implementor. */
    uint32_t abi_version;
};

#define L3D_ALLOCATOR_ABI_VERSION 1u

/** Allocator backed by the C runtime (malloc/free). Always available. */
l3d_allocator* l3d_allocator_system(void);

/** Allocator backed by a fixed size memory block owned by the caller. */
l3d_allocator* l3d_allocator_linear_create(void* memory, size_t capacity);

/** Reset a linear allocator (invalidates everything it handed out). */
void l3d_allocator_linear_reset(l3d_allocator* allocator);

/** Destroy an allocator created by one of the *_create functions above. */
void l3d_allocator_destroy(l3d_allocator* allocator);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* L3D_CORE_C_L3D_ALLOCATOR_H */
