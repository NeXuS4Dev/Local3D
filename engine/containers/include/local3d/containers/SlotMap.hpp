#pragma once
/// @file SlotMap.hpp
/// @brief Generation counted handles over a dense array.
///
/// Handles stay valid to *detect* staleness: a recycled slot bumps its
/// generation, so an old handle fails validation instead of silently pointing
/// at a different object.  This is what makes EntityHandle safe to cache in
/// gameplay code.

#include "local3d/core/Assert.hpp"
#include "local3d/core/Common.hpp"

#include <utility>
#include <vector>

namespace l3d {

/// 32 bit slot + 32 bit generation.  Invalid when both are max.
struct SlotHandle {
    u32 index = InvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return index != InvalidIndex; }
    friend constexpr bool operator==(SlotHandle a, SlotHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(SlotHandle a, SlotHandle b) noexcept { return !(a == b); }
};

inline constexpr SlotHandle kInvalidSlotHandle{};

template <typename T>
class SlotMap {
public:
    using Handle = SlotHandle;

    SlotMap() = default;

    template <typename... Args>
    Handle Emplace(Args&&... args) {
        u32 index = 0;
        u32 generation = 0;
        if (!freeList_.empty()) {
            index = freeList_.back().index;
            generation = freeList_.back().generation;
            freeList_.pop_back();
        } else {
            index = static_cast<u32>(slots_.size());
            slots_.emplace_back();
        }
        Slot& slot = slots_[index];
        slot.alive = true;
        slot.value = T(std::forward<Args>(args)...);
        return Handle{index, generation};
    }

    [[nodiscard]] bool IsAlive(Handle handle) const noexcept {
        return handle.IsValid() && handle.index < slots_.size() &&
               slots_[handle.index].alive && slots_[handle.index].generation == handle.generation;
    }

    [[nodiscard]] T* Get(Handle handle) noexcept {
        if (!IsAlive(handle)) {
            return nullptr;
        }
        return &slots_[handle.index].value;
    }

    [[nodiscard]] const T* Get(Handle handle) const noexcept {
        if (!IsAlive(handle)) {
            return nullptr;
        }
        return &slots_[handle.index].value;
    }

    bool Remove(Handle handle) {
        if (!IsAlive(handle)) {
            return false;
        }
        Slot& slot = slots_[handle.index];
        slot.value = T{};
        slot.alive = false;
        slot.generation++;
        freeList_.push_back(Handle{handle.index, slot.generation});
        return true;
    }

    [[nodiscard]] usize Size() const noexcept {
        usize count = 0;
        for (const Slot& slot : slots_) {
            if (slot.alive) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] usize Capacity() const noexcept { return slots_.size(); }

    /// Visit every live object.  The callback receives the handle and value.
    template <typename Fn>
    void ForEach(Fn&& fn) {
        for (u32 i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.alive) {
                fn(Handle{i, slot.generation}, slot.value);
            }
        }
    }

    template <typename Fn>
    void ForEach(Fn&& fn) const {
        for (u32 i = 0; i < slots_.size(); ++i) {
            const Slot& slot = slots_[i];
            if (slot.alive) {
                fn(Handle{i, slot.generation}, slot.value);
            }
        }
    }

private:
    struct Slot {
        T value{};
        u32 generation = 0;
        bool alive = false;
    };

    std::vector<Slot> slots_;
    std::vector<Handle> freeList_;
};

} // namespace l3d
