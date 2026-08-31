#include "local3d/ecs/World.hpp"

namespace l3d::ecs {

Entity World::CreateEntity() {
    u32 index = InvalidIndex;
    if (!freeIndices_.empty()) {
        index = freeIndices_.back();
        freeIndices_.pop_back();
    } else {
        index = static_cast<u32>(entityRecords_.size());
        entityRecords_.push_back(EntityRecord{});
    }
    entityRecords_[index].alive = true;
    ++aliveCount_;
    return Entity{index, entityRecords_[index].generation};
}

void World::DestroyEntity(Entity entity) {
    if (!IsAlive(entity)) {
        return;
    }
    for (auto& pool : pools_) {
        pool.second->Remove(entity.index);
    }
    EntityRecord& record = entityRecords_[entity.index];
    record.alive = false;
    record.generation++;
    freeIndices_.push_back(entity.index);
    --aliveCount_;
}

std::vector<Entity> World::AllEntities() const {
    std::vector<Entity> entities;
    entities.reserve(aliveCount_);
    for (u32 i = 0; i < entityRecords_.size(); ++i) {
        if (entityRecords_[i].alive) {
            entities.push_back(Entity{i, entityRecords_[i].generation});
        }
    }
    return entities;
}

void World::CopyComponents(Entity source, Entity target) {
    if (!IsAlive(source) || !IsAlive(target) || source.index == target.index) {
        return;
    }
    for (auto& pool : pools_) {
        pool.second->CopyComponent(source.index, target.index);
    }
}

void World::Clear() {
    for (auto& pool : pools_) {
        pool.second->Clear();
    }
    entityRecords_.clear();
    freeIndices_.clear();
    aliveCount_ = 0;
}

} // namespace l3d::ecs
