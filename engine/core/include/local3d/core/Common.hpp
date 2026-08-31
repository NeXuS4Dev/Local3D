#pragma once
/// @file Common.hpp
/// @brief Compiler plumbing, fundamental type aliases and tiny helpers shared by
///        every Local3D module.  This header must stay dependency free.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

// --- Macro utilities ------------------------------------------------------
#define L3D_STRINGIFY_IMPL(x) #x
#define L3D_STRINGIFY(x) L3D_STRINGIFY_IMPL(x)
#define L3D_CONCAT_IMPL(a, b) a##b
#define L3D_CONCAT(a, b) L3D_CONCAT_IMPL(a, b)
#define L3D_UNUSED(x) static_cast<void>(x)
#define L3D_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__has_builtin)
#    define L3D_HAS_BUILTIN(x) __has_builtin(x)
#else
#    define L3D_HAS_BUILTIN(x) 0
#endif

#if L3D_HAS_BUILTIN(__builtin_expect)
#    define L3D_LIKELY(x) __builtin_expect(static_cast<bool>(x), 1)
#    define L3D_UNLIKELY(x) __builtin_expect(static_cast<bool>(x), 0)
#else
#    define L3D_LIKELY(x) (x)
#    define L3D_UNLIKELY(x) (x)
#endif

#if L3D_HAS_BUILTIN(__builtin_unreachable)
#    define L3D_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#    define L3D_UNREACHABLE() __assume(false)
#else
#    define L3D_UNREACHABLE() std::abort()
#endif

#if defined(_MSC_VER)
#    define L3D_FORCE_INLINE __forceinline
#    define L3D_NOINLINE __declspec(noinline)
#else
#    define L3D_FORCE_INLINE inline __attribute__((always_inline))
#    define L3D_NOINLINE __attribute__((noinline))
#endif

#if defined(_MSC_VER)
#    define L3D_DEBUG_BREAK() __debugbreak()
#else
#    define L3D_DEBUG_BREAK() __builtin_trap()
#endif

/// Hint to the optimiser that `cond` always holds.  Unlike an assert this is
/// compiled into shipping builds, so only use it for genuine invariants.
#define L3D_ASSUME(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            L3D_UNREACHABLE();                                                                     \
        }                                                                                          \
    } while (false)

namespace l3d {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

/// Owning/mutable byte range.  Used everywhere data crosses a module boundary
/// without implying any particular container.
using ByteSpan = std::span<std::byte>;
using ConstByteSpan = std::span<const std::byte>;

/// Sentinel value for "no index" in u32 index spaces.
inline constexpr u32 InvalidIndex = 0xFFFFFFFFu;

/// Helper to reinterpret a typed span as bytes (used by serialization).
template <typename T>
[[nodiscard]] constexpr ConstByteSpan AsBytes(std::span<const T> data) noexcept {
    return std::as_bytes(data);
}

template <typename T>
[[nodiscard]] constexpr ByteSpan AsWritableBytes(std::span<T> data) noexcept {
    return std::as_writable_bytes(data);
}

} // namespace l3d
