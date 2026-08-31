#include "local3d/rhi/RhiDevice.hpp"

#include "local3d/core/Log.hpp"
#include "null/NullDevice.hpp"

#include <mutex>
#include <unordered_map>

namespace l3d::rhi {
namespace {

struct FactoryRegistry {
    std::mutex mutex;
    std::unordered_map<BackendType, DeviceFactory> factories;
};

FactoryRegistry& Registry() {
    static FactoryRegistry registry;
    return registry;
}

/// Built in backends are registered through static initialisers in their own
/// translation units, so linking a backend is all it takes to enable it.
class FactoryRegistrar {
public:
    FactoryRegistrar(BackendType type, DeviceFactory factory) {
        RegisterDeviceFactory(type, std::move(factory));
    }
};

} // namespace

void RegisterDeviceFactory(BackendType type, DeviceFactory factory) {
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.factories[type] = std::move(factory);
}

Result<std::unique_ptr<IDevice>> CreateDevice(const DeviceDesc& desc, bool* usedFallback) {
    if (usedFallback != nullptr) {
        *usedFallback = false;
    }
    // The null backend is always available; backends that need a driver register
    // themselves from their own translation unit.
    null::RegisterNullBackend();

    auto& registry = Registry();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto found = registry.factories.find(desc.preferredBackend);
        if (found != registry.factories.end() && found->second) {
            auto result = found->second(desc);
            if (result.HasValue()) {
                return result;
            }
            L3D_LOG_WARN(LogCategory::Rhi, "{} backend failed to initialise: {}",
                         BackendTypeName(desc.preferredBackend), result.Error().ToString());
        } else if (desc.preferredBackend != BackendType::Null) {
            L3D_LOG_WARN(LogCategory::Rhi, "{} backend is not available in this build",
                         BackendTypeName(desc.preferredBackend));
        }
    }

    if (desc.preferredBackend != BackendType::Null) {
        // The null backend is always compiled in, so the engine can always run.
        DeviceDesc fallback = desc;
        fallback.preferredBackend = BackendType::Null;
        if (usedFallback != nullptr) {
            *usedFallback = true;
        }
        auto& nullFactory = registry.factories[BackendType::Null];
        if (!nullFactory) {
            return Unexpected(Status{StatusCode::NotInitialized, "Null RHI backend is missing"});
        }
        return nullFactory(fallback);
    }

    return Unexpected(Status{StatusCode::NotInitialized, "No RHI backend registered"});
}

} // namespace l3d::rhi
