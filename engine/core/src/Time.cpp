#include "local3d/core/Time.hpp"

#include <chrono>

namespace l3d {

u64 Clock::NowNs() noexcept {
    // steady_clock is monotonic on every platform we support, which is exactly
    // what frame timing and profiling need.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace l3d
