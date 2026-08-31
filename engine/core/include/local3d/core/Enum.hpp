#pragma once
/// @file Enum.hpp
/// @brief Bit flag support for `enum class` and enum<->string helpers.

#include "local3d/core/Common.hpp"

#include <string_view>
#include <type_traits>

namespace l3d {

/// Convert to the underlying integer type.
template <typename E>
[[nodiscard]] constexpr auto ToUnderlying(E value) noexcept {
    static_assert(std::is_enum_v<E>, "ToUnderlying requires an enum");
    return static_cast<std::underlying_type_t<E>>(value);
}

/// Look up a name in a compile-time table of (value, name) pairs.
template <typename E, std::size_t N>
[[nodiscard]] constexpr std::string_view EnumName(E value,
                                                  const std::pair<E, std::string_view> (&table)[N],
                                                  std::string_view fallback = "Unknown") noexcept {
    for (const auto& entry : table) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return fallback;
}

} // namespace l3d

/// Mark `EnumType` as a bit flag set: enables |, &, ^, ~, |=, &= and the
/// HasAnyFlag/HasAllFlags helpers.  The macro works at any namespace depth
/// because it only declares functions next to the enum (found by ADL) - no
/// template specialisation, which a macro cannot express portably.
#define L3D_FLAGS_ENUM(EnumType)                                                                   \
    constexpr EnumType operator|(EnumType a, EnumType b) noexcept {                                \
        return static_cast<EnumType>(::l3d::ToUnderlying(a) | ::l3d::ToUnderlying(b));              \
    }                                                                                              \
    constexpr EnumType operator&(EnumType a, EnumType b) noexcept {                                \
        return static_cast<EnumType>(::l3d::ToUnderlying(a) & ::l3d::ToUnderlying(b));              \
    }                                                                                              \
    constexpr EnumType operator^(EnumType a, EnumType b) noexcept {                                \
        return static_cast<EnumType>(::l3d::ToUnderlying(a) ^ ::l3d::ToUnderlying(b));              \
    }                                                                                              \
    constexpr EnumType operator~(EnumType a) noexcept {                                            \
        return static_cast<EnumType>(~::l3d::ToUnderlying(a));                                      \
    }                                                                                              \
    constexpr EnumType& operator|=(EnumType& a, EnumType b) noexcept {                             \
        a = a | b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    constexpr EnumType& operator&=(EnumType& a, EnumType b) noexcept {                             \
        a = a & b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    constexpr bool HasAllFlags(EnumType set, EnumType test) noexcept {                             \
        return (set & test) == test;                                                               \
    }                                                                                              \
    constexpr bool HasAnyFlag(EnumType set, EnumType test) noexcept {                              \
        return (set & test) != static_cast<EnumType>(0);                                           \
    }
