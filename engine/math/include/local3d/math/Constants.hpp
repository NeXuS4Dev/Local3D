#pragma once
/// @file Constants.hpp
/// @brief Math constants and scalar helpers.  All constexpr, no dependencies.

#include "local3d/core/Common.hpp"

#include <cmath>
#include <limits>

namespace l3d::math {

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kHalfPi = 0.5f * kPi;
inline constexpr f32 kInvPi = 1.0f / kPi;
inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;
inline constexpr f32 kSqrt2 = 1.41421356237309504880f;
inline constexpr f32 kEpsilon = 1e-6f;
inline constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();

/// Convert degrees to radians at compile time when possible.
[[nodiscard]] constexpr f32 DegToRad(f32 degrees) noexcept { return degrees * kDegToRad; }
[[nodiscard]] constexpr f32 RadToDeg(f32 radians) noexcept { return radians * kRadToDeg; }

[[nodiscard]] constexpr f32 Clamp(f32 value, f32 min, f32 max) noexcept {
    return value < min ? min : (value > max ? max : value);
}

[[nodiscard]] constexpr f32 Clamp01(f32 value) noexcept { return Clamp(value, 0.0f, 1.0f); }

[[nodiscard]] constexpr f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

/// Hermite smoothstep between edges.
[[nodiscard]] constexpr f32 SmoothStep(f32 edge0, f32 edge1, f32 x) noexcept {
    const f32 t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] constexpr f32 InverseLerp(f32 a, f32 b, f32 value) noexcept {
    return (b - a) != 0.0f ? (value - a) / (b - a) : 0.0f;
}

/// Tolerant float equality.  Used in tests and in change detection.
[[nodiscard]] constexpr bool Approximately(f32 a, f32 b, f32 epsilon = kEpsilon) noexcept {
    const f32 diff = a - b;
    return diff < epsilon && diff > -epsilon;
}

/// Sign that maps zero to +1 (useful for stable normals).
[[nodiscard]] constexpr f32 SignOrPositive(f32 value) noexcept { return value < 0.0f ? -1.0f : 1.0f; }

/// Wrap an angle into [0, 2pi).
[[nodiscard]] inline f32 WrapAngle(f32 radians) noexcept {
    const f32 wrapped = std::fmod(radians, kTwoPi);
    return wrapped < 0.0f ? wrapped + kTwoPi : wrapped;
}

/// Wrap degrees into [-180, 180).
[[nodiscard]] inline f32 WrapDegrees(f32 degrees) noexcept {
    f32 wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped - 180.0f;
}

[[nodiscard]] constexpr u32 NextPowerOfTwo(u32 value) noexcept {
    u32 result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

/// Linear RGB -> sRGB transfer function (used when writing HDR results out).
[[nodiscard]] inline f32 LinearToSrgb(f32 linear) noexcept {
    if (linear <= 0.0031308f) {
        return linear * 12.92f;
    }
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

/// sRGB -> linear RGB.  Textures are stored as sRGB and shaded in linear.
[[nodiscard]] inline f32 SrgbToLinear(f32 srgb) noexcept {
    if (srgb <= 0.04045f) {
        return srgb / 12.92f;
    }
    return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

} // namespace l3d::math
