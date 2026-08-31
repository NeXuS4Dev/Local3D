#pragma once
/// @file Hash.hpp
/// @brief Non-cryptographic hashing used for asset ids, hash maps and
///        content addressing.  FNV-1a for strings and SplitMix64 for mixing.

#include "local3d/core/Common.hpp"

#include <cstring>
#include <string_view>

namespace l3d {

inline constexpr u64 kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr u64 kFnvPrime = 1099511628211ULL;

/// FNV-1a over a byte range.
[[nodiscard]] constexpr u64 HashBytes(ConstByteSpan data, u64 seed = kFnvOffsetBasis) noexcept {
    u64 hash = seed;
    for (const std::byte b : data) {
        hash ^= static_cast<u64>(b);
        hash *= kFnvPrime;
    }
    return hash;
}

/// FNV-1a over a string.
[[nodiscard]] constexpr u64 HashString(std::string_view text, u64 seed = kFnvOffsetBasis) noexcept {
    u64 hash = seed;
    for (const char c : text) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(c));
        hash *= kFnvPrime;
    }
    return hash;
}

/// Case-insensitive variant, used for asset path keys.
[[nodiscard]] constexpr u64 HashStringCaseInsensitive(std::string_view text) noexcept {
    u64 hash = kFnvOffsetBasis;
    for (const char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        const unsigned char lower = (uc >= 'A' && uc <= 'Z') ? static_cast<unsigned char>(uc + 32) : uc;
        hash ^= static_cast<u64>(lower);
        hash *= kFnvPrime;
    }
    return hash;
}

/// SplitMix64 - excellent avalanche, cheap, good for combining hashes.
[[nodiscard]] constexpr u64 MixHash(u64 value) noexcept {
    u64 z = value + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// Combine two hashes order-dependently.
[[nodiscard]] constexpr u64 HashCombine(u64 a, u64 b) noexcept {
    return MixHash(a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2)));
}

/// Combine an arbitrary number of hashes.
template <typename... Args>
[[nodiscard]] constexpr u64 HashOf(Args... args) noexcept {
    u64 hash = kFnvOffsetBasis;
    ((hash = HashCombine(hash, static_cast<u64>(args))), ...);
    return hash;
}

/// Transparent hasher usable with std::unordered_map<string, ...>.
struct StringHash {
    using is_transparent = void;
    [[nodiscard]] u64 operator()(std::string_view text) const noexcept {
        return HashString(text);
    }
};

} // namespace l3d
