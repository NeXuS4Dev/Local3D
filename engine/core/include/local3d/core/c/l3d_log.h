#ifndef L3D_CORE_C_L3D_LOG_H
#define L3D_CORE_C_L3D_LOG_H
/* ---------------------------------------------------------------------------
 * Local3D logging - C ABI.
 *
 * Purpose: give C translation units, plugins and other languages a stable,
 * exception free way to emit and observe engine logs.  The C++ Logger remains
 * the authoritative implementation; this layer forwards to it.
 *
 * ABI rules honoured here:
 *   - only fixed width integer types, pointers and enums;
 *   - no C++ types, no exceptions crossing the boundary;
 *   - explicit ownership: every callback receives an opaque user pointer that
 *     the caller owns, and the engine never frees it.
 * ------------------------------------------------------------------------- */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum l3d_log_level {
    L3D_LOG_LEVEL_TRACE = 0,
    L3D_LOG_LEVEL_DEBUG = 1,
    L3D_LOG_LEVEL_INFO = 2,
    L3D_LOG_LEVEL_WARNING = 3,
    L3D_LOG_LEVEL_ERROR = 4,
    L3D_LOG_LEVEL_FATAL = 5,
    L3D_LOG_LEVEL_OFF = 6
} l3d_log_level;

typedef enum l3d_log_category {
    L3D_LOG_CATEGORY_CORE = 0,
    L3D_LOG_CATEGORY_MEMORY = 1,
    L3D_LOG_CATEGORY_PLATFORM = 2,
    L3D_LOG_CATEGORY_ASSETS = 3,
    L3D_LOG_CATEGORY_RENDERER = 4,
    L3D_LOG_CATEGORY_PHYSICS = 5,
    L3D_LOG_CATEGORY_AUDIO = 6,
    L3D_LOG_CATEGORY_EDITOR = 7,
    L3D_LOG_CATEGORY_RUNTIME = 8
} l3d_log_category;

/** One log record as seen by an observer callback. */
typedef struct l3d_log_record {
    int32_t level;
    int32_t category;
    const char* text;     /**< Valid only for the duration of the callback. */
    uint64_t timestamp_ns;
    uint32_t thread_id;
} l3d_log_record;

/** Observer callback. Must not call back into l3d_log_write (no reentrancy). */
typedef void (*l3d_log_callback)(const l3d_log_record* record, void* user_data);

/** Emit a log message. `message` must be NUL terminated and non-null. */
void l3d_log_write(int32_t level, int32_t category, const char* message);

/** Install an observer. Returns a non-zero handle, or 0 on failure. */
uint64_t l3d_log_add_observer(l3d_log_callback callback, void* user_data);

/** Remove a previously installed observer. */
void l3d_log_remove_observer(uint64_t handle);

/** Set the global minimum level. */
void l3d_log_set_level(int32_t level);

/** Human readable name of a level; never returns NULL. */
const char* l3d_log_level_name(int32_t level);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* L3D_CORE_C_L3D_LOG_H */
