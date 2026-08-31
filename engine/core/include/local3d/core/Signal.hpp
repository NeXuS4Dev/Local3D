#pragma once
/// @file Signal.hpp
/// @brief Minimal multicast observer used for engine events (asset reloaded,
///        selection changed, scene modified, ...).
///
/// Not a general purpose event bus: it is intentionally synchronous and
/// single-threaded (fire on the thread that raised the event).

#include "local3d/core/Common.hpp"

#include <functional>
#include <vector>

namespace l3d {

/// Handle returned by Signal::Connect, used to disconnect.
class SignalConnection {
public:
    SignalConnection() = default;
    explicit SignalConnection(u64 id) noexcept : id_(id) {}
    [[nodiscard]] constexpr u64 Id() const noexcept { return id_; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return id_ != 0; }
    friend constexpr bool operator==(SignalConnection a, SignalConnection b) noexcept {
        return a.id_ == b.id_;
    }

private:
    u64 id_ = 0;
};

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    SignalConnection Connect(Slot slot) {
        const u64 id = nextId_++;
        slots_.push_back(SlotEntry{id, std::move(slot)});
        return SignalConnection{id};
    }

    void Disconnect(SignalConnection connection) {
        for (usize i = 0; i < slots_.size(); ++i) {
            if (slots_[i].id == connection.Id()) {
                slots_.erase(slots_.begin() + static_cast<isize>(i));
                return;
            }
        }
    }

    void DisconnectAll() { slots_.clear(); }

    [[nodiscard]] usize SlotCount() const noexcept { return slots_.size(); }

    /// Invoke every slot with the given arguments, in connection order.
    void Emit(Args... args) const {
        // Copy first: a slot is allowed to disconnect itself or others.
        const std::vector<SlotEntry> snapshot = slots_;
        for (const SlotEntry& entry : snapshot) {
            if (entry.fn) {
                entry.fn(args...);
            }
        }
    }

private:
    struct SlotEntry {
        u64 id = 0;
        Slot fn;
    };

    std::vector<SlotEntry> slots_;
    u64 nextId_ = 1;
};

} // namespace l3d
