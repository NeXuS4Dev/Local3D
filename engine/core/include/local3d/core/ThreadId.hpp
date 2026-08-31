#pragma once
/// @file ThreadId.hpp
/// @brief Cheap 32 bit thread identifiers for logs, markers and job accounting.

#include "local3d/core/Common.hpp"

#include <functional>
#include <thread>

namespace l3d {

/// Stable for the lifetime of the thread, unique enough for diagnostics.  This
/// is *not* a slot index - the job system keeps its own worker indices.
[[nodiscard]] inline u32 CurrentThreadId() noexcept {
    const std::size_t hashed = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<u32>(hashed & 0xFFFFFFFFu);
}

} // namespace l3d
