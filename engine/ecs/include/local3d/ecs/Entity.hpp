#pragma once
/// @file Entity.hpp
/// @brief Entity handles and component identity.

#include "local3d/core/Common.hpp"

#include <functional>

namespace l3d::ecs {

/// A stable reference to an entity.  The generation counter makes stale handles
/// detectable: after DestroyEntity, the index may be reused but the generation
/// changes, so old handles fail validation instead of aliasing a new entity.
struct Entity {
    u32 index = InvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return index != InvalidIndex; }
    friend constexpr bool operator==(Entity a, Entity b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(Entity a, Entity b) noexcept { return !(a == b); }
    /// Deterministic ordering for maps and stable iteration.
    friend constexpr bool operator<(Entity a, Entity b) noexcept {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    }
};

inline constexpr Entity kNullEntity{};

/// Component identity.  Derived from the address of a static sentinel inside a
/// template, so it is unique per type within the process and costs nothing to
/// compute.  Cross-process identity (scene files, prefabs) uses the type *name*
/// through the reflection registry instead - see docs/architecture/ecs.md.
using ComponentId = u64;

template <typename T>
[[nodiscard]] inline ComponentId ComponentIdOf() noexcept {
    static const char sentinel = 0;
    return reinterpret_cast<ComponentId>(&sentinel);
}

} // namespace l3d::ecs

namespace std {
template <>
struct hash<l3d::ecs::Entity> {
    [[nodiscard]] size_t operator()(const l3d::ecs::Entity& entity) const noexcept {
        return static_cast<size_t>(entity.index) * 2654435761u + entity.generation;
    }
};
} // namespace std
