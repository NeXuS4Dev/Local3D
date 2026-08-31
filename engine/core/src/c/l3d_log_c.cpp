#include "local3d/core/c/l3d_log.h"

#include "local3d/core/Log.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>

// C ABI shim.  Nothing here may throw across the boundary, and no C++ type may
// appear in the public header - only fixed width integers and pointers.

namespace {

struct ObserverRegistry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::pair<l3d_log_callback, void*>> observers;
    std::uint64_t nextHandle = 1;
};

[[nodiscard]] ObserverRegistry& Registry() {
    static ObserverRegistry registry;
    return registry;
}

void Dispatch(const l3d::LogMessage& message) {
    l3d_log_record record{};
    record.level = static_cast<std::int32_t>(message.level);
    record.category = static_cast<std::int32_t>(message.category);
    record.text = message.text.data();
    record.timestamp_ns = message.timestampNs;
    record.thread_id = message.threadId;

    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (const auto& entry : registry.observers) {
        if (entry.second.first != nullptr) {
            entry.second.first(&record, entry.second.second);
        }
    }
}

/// Install the C++ -> C bridge exactly once.
bool EnsureBridgeInstalled() {
    static std::once_flag flag;
    static bool installed = false;
    std::call_once(flag, [] {
        l3d::Logger::Instance().AddSink(&Dispatch);
        installed = true;
    });
    return installed;
}

} // namespace

extern "C" {

void l3d_log_write(std::int32_t level, std::int32_t category, const char* message) {
    if (message == nullptr) {
        return;
    }
    l3d::Logger::Instance().Write(static_cast<l3d::LogLevel>(level),
                                  static_cast<l3d::LogCategory>(category),
                                  std::string_view(message));
}

std::uint64_t l3d_log_add_observer(l3d_log_callback callback, void* user_data) {
    if (callback == nullptr || !EnsureBridgeInstalled()) {
        return 0;
    }
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const std::uint64_t handle = registry.nextHandle++;
    registry.observers[handle] = {callback, user_data};
    return handle;
}

void l3d_log_remove_observer(std::uint64_t handle) {
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.observers.erase(handle);
}

void l3d_log_set_level(std::int32_t level) {
    l3d::Logger::Instance().SetLevel(static_cast<l3d::LogLevel>(level));
}

const char* l3d_log_level_name(std::int32_t level) {
    static thread_local char buffer[16]{};
    const std::string_view name = l3d::LogLevelName(static_cast<l3d::LogLevel>(level));
    const std::size_t count = name.size() < sizeof(buffer) - 1 ? name.size() : sizeof(buffer) - 1;
    for (std::size_t i = 0; i < count; ++i) {
        buffer[i] = name[i];
    }
    buffer[count] = '\0';
    return buffer;
}

} // extern "C"
