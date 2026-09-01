#include "local3d/physics/PhysicsWorld.hpp"

#include <utility>
#include <vector>

namespace l3d::physics {
namespace {

/// The default backend, and the one every fallback lands on.
constexpr std::string_view kDefaultBackend = "Simple";

struct FactoryEntry {
    std::string name;
    PhysicsWorldFactory create = nullptr;
};

/// Backends by name.
///
/// A function local static rather than a global: registration is explicit and
/// idempotent, so a backend that lives in an unreferenced object file of a static
/// library is still registered by the time it is asked for.  The RHI does the
/// same thing for the same reason - see docs/architecture/rhi.md.
[[nodiscard]] std::vector<FactoryEntry>& Factories() {
    static std::vector<FactoryEntry> entries;
    return entries;
}

[[nodiscard]] PhysicsWorldFactory FindFactory(std::string_view name) {
    for (const FactoryEntry& entry : Factories()) {
        if (entry.name == name) {
            return entry.create;
        }
    }
    return nullptr;
}

} // namespace

/// Defined in simple/SimpleWorld.cpp.
[[nodiscard]] Result<std::unique_ptr<IPhysicsWorld>> CreateSimpleWorld(const PhysicsWorldDesc& desc);

void RegisterPhysicsFactory(std::string_view name, PhysicsWorldFactory factory) {
    if (name.empty() || factory == nullptr) {
        return;
    }
    for (FactoryEntry& entry : Factories()) {
        if (entry.name == name) {
            entry.create = factory;
            return;
        }
    }
    Factories().push_back(FactoryEntry{std::string(name), factory});
}

Result<std::unique_ptr<IPhysicsWorld>> CreatePhysicsWorld(const PhysicsWorldDesc& desc,
                                                         bool* outFallback) {
    RegisterSimpleBackend();
    if (outFallback != nullptr) {
        *outFallback = false;
    }

    const std::string_view requested =
        desc.preferredBackend.empty() ? kDefaultBackend : desc.preferredBackend;
    if (const PhysicsWorldFactory factory = FindFactory(requested)) {
        return factory(desc);
    }

    // An unknown backend is a configuration mistake, not a reason to fail start
    // up: the game still runs, and the caller is told which happened.
    const PhysicsWorldFactory fallback = FindFactory(kDefaultBackend);
    if (fallback == nullptr) {
        return Unexpected(
            Status{StatusCode::Unsupported, "No physics backend is registered"});
    }
    if (outFallback != nullptr) {
        *outFallback = true;
    }
    return fallback(desc);
}

} // namespace l3d::physics
