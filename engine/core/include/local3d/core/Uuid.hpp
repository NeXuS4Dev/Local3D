#pragma once
/// @file Uuid.hpp
/// @brief 128 bit identifiers with a stable textual form.
///
/// Asset ids must survive renames, so they are random (or explicitly derived
/// from a stable name) rather than path hashes.  See
/// docs/architecture/assets.md for the identity model.

#include "local3d/core/Common.hpp"
#include "local3d/core/Hash.hpp"

#include <string>
#include <string_view>

namespace l3d {

/// A 128 bit identifier.  Value type, trivially copyable, no allocation.
class Uuid {
public:
    constexpr Uuid() noexcept = default;
    constexpr Uuid(u64 high, u64 low) noexcept : high_(high), low_(low) {}

    /// Deterministic id derived from a stable name (used for engine built-in
    /// assets so they have the same id in every process).
    [[nodiscard]] static constexpr Uuid FromName(std::string_view name) noexcept {
        return Uuid{HashString(name, kFnvOffsetBasis),
                    HashString(name, 0xCBF29CE484222325ULL)};
    }

    /// Random id (version 4 style).  Uses a thread local RNG.
    [[nodiscard]] static Uuid Generate() noexcept;

    /// Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".  Returns false if malformed.
    [[nodiscard]] static bool Parse(std::string_view text, Uuid& out) noexcept;

    [[nodiscard]] constexpr u64 High() const noexcept { return high_; }
    [[nodiscard]] constexpr u64 Low() const noexcept { return low_; }
    [[nodiscard]] constexpr bool IsNull() const noexcept { return high_ == 0 && low_ == 0; }

    /// Canonical 36 character textual form.
    [[nodiscard]] std::string ToString() const;
    /// Writes the textual form into a caller provided buffer; returns length.
    usize ToString(char* buffer, usize bufferSize) const noexcept;

    [[nodiscard]] constexpr u64 Hash() const noexcept { return HashCombine(high_, low_); }

    friend constexpr bool operator==(const Uuid& a, const Uuid& b) noexcept {
        return a.high_ == b.high_ && a.low_ == b.low_;
    }
    friend constexpr bool operator!=(const Uuid& a, const Uuid& b) noexcept { return !(a == b); }
    /// Stable ordering for deterministic iteration (maps, cook output).
    friend constexpr auto operator<=>(const Uuid& a, const Uuid& b) noexcept {
        if (a.high_ != b.high_) {
            return a.high_ < b.high_ ? -1 : 1;
        }
        if (a.low_ == b.low_) {
            return 0;
        }
        return a.low_ < b.low_ ? -1 : 1;
    }

private:
    u64 high_ = 0;
    u64 low_ = 0;
};

inline constexpr Uuid kNullUuid{};

[[nodiscard]] std::string to_string(const Uuid& uuid);

} // namespace l3d

namespace std {
template <>
struct hash<l3d::Uuid> {
    [[nodiscard]] std::size_t operator()(const l3d::Uuid& uuid) const noexcept {
        return static_cast<std::size_t>(uuid.Hash());
    }
};
} // namespace std
