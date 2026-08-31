#pragma once
/// @file SparseSet.hpp
/// @brief O(1) insert/remove/contains over a dense array of values.
///
/// This is the backbone of the ECS: iterating components is a linear scan over
/// contiguous memory (cache friendly) while lookups by sparse id stay O(1).
///
/// Layout:
///   sparse_[id]  -> index into dense_ (or InvalidIndex)
///   dense_[i]    -> { id, value }
///
/// Removal swaps the last element into the hole, so iteration order is not
/// stable across removals.  Thread safety: none.

#include "local3d/core/Assert.hpp"
#include "local3d/core/Common.hpp"

#include <utility>
#include <vector>

namespace l3d {

template <typename T>
class SparseSet {
public:
    struct Entry {
        u32 id = InvalidIndex;
        T value{};
    };

    using iterator = typename std::vector<Entry>::iterator;
    using const_iterator = typename std::vector<Entry>::const_iterator;

    SparseSet() = default;

    /// Insert or overwrite the value for `id`.  Returns a reference to it.
    T& InsertOrAssign(u32 id, T value) {
        EnsureSparse(id);
        const u32 existing = sparse_[id];
        if (existing != InvalidIndex) {
            dense_[existing].value = std::move(value);
            return dense_[existing].value;
        }
        dense_.push_back(Entry{id, std::move(value)});
        sparse_[id] = static_cast<u32>(dense_.size() - 1);
        return dense_.back().value;
    }

    [[nodiscard]] bool Contains(u32 id) const noexcept {
        return id < sparse_.size() && sparse_[id] != InvalidIndex;
    }

    [[nodiscard]] T* Find(u32 id) noexcept {
        if (id >= sparse_.size() || sparse_[id] == InvalidIndex) {
            return nullptr;
        }
        return &dense_[sparse_[id]].value;
    }

    [[nodiscard]] const T* Find(u32 id) const noexcept {
        if (id >= sparse_.size() || sparse_[id] == InvalidIndex) {
            return nullptr;
        }
        return &dense_[sparse_[id]].value;
    }

    [[nodiscard]] bool Remove(u32 id) {
        if (!Contains(id)) {
            return false;
        }
        const u32 index = sparse_[id];
        const u32 last = static_cast<u32>(dense_.size() - 1);
        if (index != last) {
            dense_[index] = std::move(dense_[last]);
            sparse_[dense_[index].id] = index;
        }
        dense_.pop_back();
        sparse_[id] = InvalidIndex;
        return true;
    }

    [[nodiscard]] u32 IndexOf(u32 id) const noexcept {
        return id < sparse_.size() ? sparse_[id] : InvalidIndex;
    }

    void Clear() {
        dense_.clear();
        for (u32& slot : sparse_) {
            slot = InvalidIndex;
        }
    }

    [[nodiscard]] usize Size() const noexcept { return dense_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return dense_.empty(); }

    [[nodiscard]] iterator begin() noexcept { return dense_.begin(); }
    [[nodiscard]] iterator end() noexcept { return dense_.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return dense_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return dense_.end(); }

    /// Contiguous id array - ideal for data oriented iteration.
    [[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return dense_; }

private:
    void EnsureSparse(u32 id) {
        if (id >= sparse_.size()) {
            sparse_.resize(static_cast<usize>(id) + 1, InvalidIndex);
        }
    }

    std::vector<u32> sparse_;
    std::vector<Entry> dense_;
};

} // namespace l3d
