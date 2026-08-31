#pragma once
/// @file Color.hpp
/// @brief Linear space RGBA colour plus the conversions the renderer needs.

#include "local3d/math/Vector.hpp"

namespace l3d::math {

/// Linear RGBA in [0,1] for albedo/emissive, unbounded for HDR values.
struct Color {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;

    constexpr Color() noexcept = default;
    constexpr Color(f32 inR, f32 inG, f32 inB, f32 inA = 1.0f) noexcept
        : r(inR), g(inG), b(inB), a(inA) {}

    [[nodiscard]] constexpr Vec3 Rgb() const noexcept { return {r, g, b}; }
    [[nodiscard]] constexpr Vec4 ToVec4() const noexcept { return {r, g, b, a}; }

    /// Build from sRGB 0..1 components (as authored in the editor / glTF).
    [[nodiscard]] static inline Color FromSrgb(f32 sr, f32 sg, f32 sb, f32 alpha = 1.0f) noexcept {
        return {SrgbToLinear(sr), SrgbToLinear(sg), SrgbToLinear(sb), alpha};
    }

    /// Build from 8 bit sRGB components.
    [[nodiscard]] static inline Color FromSrgb8(u8 sr, u8 sg, u8 sb, u8 sa = 255) noexcept {
        return FromSrgb(static_cast<f32>(sr) / 255.0f, static_cast<f32>(sg) / 255.0f,
                        static_cast<f32>(sb) / 255.0f, static_cast<f32>(sa) / 255.0f);
    }

    /// Scale RGB only (alpha is not part of lighting).
    [[nodiscard]] constexpr Color Multiplied(f32 scale) const noexcept {
        return {r * scale, g * scale, b * scale, a};
    }

    friend constexpr Color operator+(Color a, Color b) noexcept {
        return {a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a};
    }
    friend constexpr Color operator*(Color a, Color b) noexcept {
        return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a};
    }
    friend constexpr bool operator==(Color a, Color b) noexcept {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    static constexpr Color Black() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }
    static constexpr Color White() noexcept { return {1.0f, 1.0f, 1.0f, 1.0f}; }
    static constexpr Color Red() noexcept { return {1.0f, 0.0f, 0.0f, 1.0f}; }
    static constexpr Color Green() noexcept { return {0.0f, 1.0f, 0.0f, 1.0f}; }
    static constexpr Color Blue() noexcept { return {0.0f, 0.0f, 1.0f, 1.0f}; }
};

} // namespace l3d::math
