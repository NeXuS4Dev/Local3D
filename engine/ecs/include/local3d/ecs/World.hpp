#pragma once
/// @file World.hpp
/// @brief Entity component store.
///
/// Layout: one sparse set per component type (see Containers::SparseSet), so
/// iterating a single component is a linear scan over contiguous memory.
/// Entities are indices into a generation table.
///
/// Threading model (important, and enforced by convention + asserts):
///  * Structural changes (CreateEntity, DestroyEntity, Emplace, Remove) happen
///    on the main thread only.
///  * Component reads/writes may happen on worker threads as long as two systems
///    running in parallel never touch the same component type.  The scheduler
///    in System.hpp is responsible for that grouping.

#include "local3d/containers/SparseSet.hpp"
#include "local3d/core/Assert.hpp"
#include "local3d/ecs/Entity.hpp"

#include <array>
#include <functional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace l3d::ecs {

/// Type erased view onto one component pool.  The engine uses this for
/// serialization, editor tooling and generic destruction.
class IComponentPool {
public:
    virtual ~IComponentPool() = default;

    [[nodiscard]] virtual ComponentId Id() const noexcept = 0;
    [[nodiscard]] virtual usize ComponentSize() const noexcept = 0;
    [[nodiscard]] virtual usize Count() const noexcept = 0;
    [[nodiscard]] virtual bool Has(u32 entityIndex) const noexcept = 0;
    [[nodiscard]] virtual void* GetRaw(u32 entityIndex) noexcept = 0;
    [[nodiscard]] virtual const void* GetRaw(u32 entityIndex) const noexcept = 0;
    virtual void Remove(u32 entityIndex) = 0;
    /// Visit every (entityIndex, component) pair.
    virtual void ForEachRaw(const std::function<void(u32, void*)>& body) = 0;
    /// Destroy every component.  Used when a world is torn down or a scene unloaded.
    virtual void Clear() = 0;
    /// Copy one entity's component onto another entity within this pool.
    /// Does nothing when the source entity has no such component.
    virtual void CopyComponent(u32 fromEntityIndex, u32 toEntityIndex) = 0;
};

template <typename T>
class ComponentPool final : public IComponentPool {
public:
    [[nodiscard]] ComponentId Id() const noexcept override { return ComponentIdOf<T>(); }
    [[nodiscard]] usize ComponentSize() const noexcept override { return sizeof(T); }
    [[nodiscard]] usize Count() const noexcept override { return storage_.Size(); }
    [[nodiscard]] bool Has(u32 entityIndex) const noexcept override {
        return storage_.Contains(entityIndex);
    }

    [[nodiscard]] void* GetRaw(u32 entityIndex) noexcept override { return storage_.Find(entityIndex); }
    [[nodiscard]] const void* GetRaw(u32 entityIndex) const noexcept override {
        return storage_.Find(entityIndex);
    }

    void Remove(u32 entityIndex) override {
        // The bool result is not actionable here: removal is best effort.
        static_cast<void>(storage_.Remove(entityIndex));
    }

    void ForEachRaw(const std::function<void(u32, void*)>& body) override {
        for (auto& entry : storage_) {
            body(entry.id, &entry.value);
        }
    }

    void Clear() override { storage_.Clear(); }

    void CopyComponent(u32 fromEntityIndex, u32 toEntityIndex) override {
        if (const T* value = storage_.Find(fromEntityIndex)) {
            storage_.InsertOrAssign(toEntityIndex, *value);
        }
    }

    /// Typed fast paths used by the templated World API.
    template <typename... Args>
    T& Emplace(u32 entityIndex, Args&&... args) {
        return storage_.InsertOrAssign(entityIndex, T(std::forward<Args>(args)...));
    }

    [[nodiscard]] T* Get(u32 entityIndex) noexcept { return storage_.Find(entityIndex); }
    [[nodiscard]] const T* Get(u32 entityIndex) const noexcept { return storage_.Find(entityIndex); }
    [[nodiscard]] SparseSet<T>& Storage() noexcept { return storage_; }
    [[nodiscard]] const SparseSet<T>& Storage() const noexcept { return storage_; }

private:
    SparseSet<T> storage_;
};

/// The entity component world.  Owns all entities and component pools.
class World {
public:
    World() = default;
    ~World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;

    // --- Entities ---------------------------------------------------------
    [[nodiscard]] Entity CreateEntity();

    /// Destroy an entity and every component attached to it.
    void DestroyEntity(Entity entity);

    [[nodiscard]] bool IsAlive(Entity entity) const noexcept {
        return entity.IsValid() && entity.index < entityRecords_.size() &&
               entityRecords_[entity.index].alive &&
               entityRecords_[entity.index].generation == entity.generation;
    }

    [[nodiscard]] usize EntityCount() const noexcept { return aliveCount_; }

    /// Every live entity, in index order (stable for tools and serialization).
    [[nodiscard]] std::vector<Entity> AllEntities() const;

    // --- Components -------------------------------------------------------
    template <typename T, typename... Args>
    T& Emplace(Entity entity, Args&&... args) {
        L3D_ASSERT_MSG(IsAlive(entity), "Emplace on a dead entity");
        return TypedPool<T>().Emplace(entity.index, std::forward<Args>(args)...);
    }

    template <typename T>
    [[nodiscard]] T* Get(Entity entity) noexcept {
        if (!IsAlive(entity)) {
            return nullptr;
        }
        return TypedPool<T>().Get(entity.index);
    }

    template <typename T>
    [[nodiscard]] const T* Get(Entity entity) const noexcept {
        if (!IsAlive(entity)) {
            return nullptr;
        }
        const auto found = pools_.find(ComponentIdOf<T>());
        if (found == pools_.end()) {
            return nullptr;
        }
        return static_cast<const T*>(found->second->GetRaw(entity.index));
    }

    template <typename T>
    [[nodiscard]] bool Has(Entity entity) const noexcept {
        return Get<T>(entity) != nullptr;
    }

    template <typename T>
    bool Remove(Entity entity) {
        if (!IsAlive(entity)) {
            return false;
        }
        auto found = pools_.find(ComponentIdOf<T>());
        if (found == pools_.end()) {
            return false;
        }
        if (!found->second->Has(entity.index)) {
            return false;
        }
        found->second->Remove(entity.index);
        return true;
    }

    template <typename T>
    [[nodiscard]] usize ComponentCount() const noexcept {
        const auto found = pools_.find(ComponentIdOf<T>());
        return found != pools_.end() ? found->second->Count() : 0;
    }

    template <typename T>
    [[nodiscard]] ComponentPool<T>& TypedPool() {
        const ComponentId id = ComponentIdOf<T>();
        auto found = pools_.find(id);
        if (found != pools_.end()) {
            return static_cast<ComponentPool<T>&>(*found->second);
        }
        auto pool = std::make_unique<ComponentPool<T>>();
        ComponentPool<T>* raw = pool.get();
        pools_.emplace(id, std::move(pool));
        return *raw;
    }

    /// Every pool, for serialization and editor tooling.
    [[nodiscard]] std::vector<IComponentPool*> Pools() {
        std::vector<IComponentPool*> pools;
        pools.reserve(pools_.size());
        for (auto& entry : pools_) {
            pools.push_back(entry.second.get());
        }
        return pools;
    }

    /// Single component iteration: `world.Each<Transform>([](Entity, Transform&) {...})`.
    template <typename T, typename Fn>
    void Each(Fn&& fn) {
        auto& pool = TypedPool<T>();
        for (auto& entry : pool.Storage()) {
            fn(MakeEntity(entry.id), entry.value);
        }
    }

    template <typename T, typename Fn>
    void Each(Fn&& fn) const {
        const auto found = pools_.find(ComponentIdOf<T>());
        if (found == pools_.end()) {
            return;
        }
        found->second->ForEachRaw([this, &fn](u32 index, void* value) {
            fn(MakeEntity(index), *static_cast<T*>(value));
        });
    }

    /// Multi component iteration.  Visits entities that have *all* of Ts.
    /// The body receives (Entity, Ts&...).
    template <typename... Ts, typename Fn>
    void ForEach(Fn&& fn) {
        std::array<IComponentPool*, sizeof...(Ts)> pools{
            static_cast<IComponentPool*>(&TypedPool<Ts>())...};
        ForEachImpl<Ts...>(fn, pools, std::index_sequence_for<Ts...>{});
    }

    /// Copy every component of `source` onto `target` (used by prefabs and the
    /// editor's duplicate command).
    void CopyComponents(Entity source, Entity target);

    /// Remove all entities and components.
    void Clear();

private:
    struct EntityRecord {
        u32 generation = 0;
        bool alive = false;
    };

    [[nodiscard]] Entity MakeEntity(u32 index) const noexcept {
        return Entity{index, entityRecords_[index].generation};
    }

    template <typename... Ts, typename Fn, std::size_t... Is>
    void ForEachImpl(Fn& fn, std::array<IComponentPool*, sizeof...(Ts)>& pools,
                     std::index_sequence<Is...> /*indices*/) {
        // Pick the sparsest pool as the driver: fewer iterations, same result.
        usize driver = 0;
        usize smallest = pools[0]->Count();
        for (usize i = 1; i < sizeof...(Ts); ++i) {
            if (pools[i]->Count() < smallest) {
                smallest = pools[i]->Count();
                driver = i;
            }
        }
        pools[driver]->ForEachRaw([&fn, &pools, this](u32 index, void*) {
            void* components[] = {pools[Is]->GetRaw(index)...};
            if (((components[Is] != nullptr) && ...)) {
                fn(MakeEntity(index), *static_cast<Ts*>(components[Is])...);
            }
        });
    }

    std::vector<EntityRecord> entityRecords_;
    std::vector<u32> freeIndices_;
    usize aliveCount_ = 0;
    std::unordered_map<ComponentId, std::unique_ptr<IComponentPool>> pools_;
};

} // namespace l3d::ecs
