#pragma once
/// @file Vector.hpp
/// @brief Vec2/Vec3/Vec4 - value types, no allocation, no exceptions.
///
/// Conventions:
///  * Right handed coordinate system, Y up, -Z forward (matches glTF and the
///    editor camera).
///  * All operations are constexpr where the standard allows it.
///  * No implicit conversions to/from pointers; use Data() explicitly.

#include "local3d/core/Common.hpp"
#include "local3d/math/Constants.hpp"

#include <cmath>

namespace l3d::math {

struct Vec2;
struct Vec3;
struct Vec4;

/// 2 component vector.  Mostly used for texture coordinates and screen space.
struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    constexpr Vec2() noexcept = default;
    constexpr Vec2(f32 inX, f32 inY) noexcept : x(inX), y(inY) {}
    explicit constexpr Vec2(f32 scalar) noexcept : x(scalar), y(scalar) {}

    [[nodiscard]] constexpr const f32* Data() const noexcept { return &x; }

    [[nodiscard]] constexpr f32& operator[](usize index) noexcept { return (&x)[index]; }
    [[nodiscard]] constexpr f32 operator[](usize index) const noexcept { return (&x)[index]; }

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, Vec2 b) noexcept { return {a.x * b.x, a.y * b.y}; }
    friend constexpr Vec2 operator/(Vec2 a, Vec2 b) noexcept { return {a.x / b.x, a.y / b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, f32 s) noexcept { return {a.x * s, a.y * s}; }
    friend constexpr Vec2 operator*(f32 s, Vec2 a) noexcept { return {a.x * s, a.y * s}; }
    friend constexpr Vec2 operator/(Vec2 a, f32 s) noexcept { return {a.x / s, a.y / s}; }
    friend constexpr Vec2 operator-(Vec2 a) noexcept { return {-a.x, -a.y}; }

    constexpr Vec2& operator+=(Vec2 b) noexcept { x += b.x; y += b.y; return *this; }
    constexpr Vec2& operator-=(Vec2 b) noexcept { x -= b.x; y -= b.y; return *this; }
    constexpr Vec2& operator*=(f32 s) noexcept { x *= s; y *= s; return *this; }

    friend constexpr bool operator==(Vec2 a, Vec2 b) noexcept { return a.x == b.x && a.y == b.y; }
};

/// 3 component vector: positions, directions, normals, scales.
struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    constexpr Vec3() noexcept = default;
    constexpr Vec3(f32 inX, f32 inY, f32 inZ) noexcept : x(inX), y(inY), z(inZ) {}
    explicit constexpr Vec3(f32 scalar) noexcept : x(scalar), y(scalar), z(scalar) {}

    [[nodiscard]] constexpr const f32* Data() const noexcept { return &x; }

    [[nodiscard]] constexpr f32& operator[](usize index) noexcept { return (&x)[index]; }
    [[nodiscard]] constexpr f32 operator[](usize index) const noexcept { return (&x)[index]; }

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    friend constexpr Vec3 operator*(Vec3 a, Vec3 b) noexcept {
        return {a.x * b.x, a.y * b.y, a.z * b.z};
    }
    friend constexpr Vec3 operator/(Vec3 a, Vec3 b) noexcept {
        return {a.x / b.x, a.y / b.y, a.z / b.z};
    }
    friend constexpr Vec3 operator*(Vec3 a, f32 s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr Vec3 operator*(f32 s, Vec3 a) noexcept { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr Vec3 operator/(Vec3 a, f32 s) noexcept { return {a.x / s, a.y / s, a.z / s}; }
    friend constexpr Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }

    constexpr Vec3& operator+=(Vec3 b) noexcept {
        x += b.x; y += b.y; z += b.z; return *this;
    }
    constexpr Vec3& operator-=(Vec3 b) noexcept {
        x -= b.x; y -= b.y; z -= b.z; return *this;
    }
    constexpr Vec3& operator*=(f32 s) noexcept {
        x *= s; y *= s; z *= s; return *this;
    }
    constexpr Vec3& operator/=(f32 s) noexcept {
        x /= s; y /= s; z /= s; return *this;
    }

    friend constexpr bool operator==(Vec3 a, Vec3 b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    static constexpr Vec3 Zero() noexcept { return {0.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 One() noexcept { return {1.0f, 1.0f, 1.0f}; }
    static constexpr Vec3 Up() noexcept { return {0.0f, 1.0f, 0.0f}; }
    static constexpr Vec3 Down() noexcept { return {0.0f, -1.0f, 0.0f}; }
    static constexpr Vec3 Right() noexcept { return {1.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 Left() noexcept { return {-1.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 Forward() noexcept { return {0.0f, 0.0f, -1.0f}; }
    static constexpr Vec3 Back() noexcept { return {0.0f, 0.0f, 1.0f}; }
};

/// 4 component vector: homogeneous positions, colours, shader data.
struct Vec4 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr Vec4() noexcept = default;
    constexpr Vec4(f32 inX, f32 inY, f32 inZ, f32 inW) noexcept : x(inX), y(inY), z(inZ), w(inW) {}
    constexpr Vec4(Vec3 xyz, f32 inW) noexcept : x(xyz.x), y(xyz.y), z(xyz.z), w(inW) {}
    explicit constexpr Vec4(f32 scalar) noexcept : x(scalar), y(scalar), z(scalar), w(scalar) {}

    [[nodiscard]] constexpr Vec3 Xyz() const noexcept { return {x, y, z}; }
    [[nodiscard]] constexpr const f32* Data() const noexcept { return &x; }

    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    friend constexpr Vec4 operator*(Vec4 a, f32 s) noexcept {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }
    friend constexpr Vec4 operator*(f32 s, Vec4 a) noexcept {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }
    friend constexpr Vec4 operator/(Vec4 a, f32 s) noexcept {
        return {a.x / s, a.y / s, a.z / s, a.w / s};
    }
    friend constexpr Vec4 operator-(Vec4 a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }
    friend constexpr bool operator==(Vec4 a, Vec4 b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
};

// --- Free functions --------------------------------------------------------

[[nodiscard]] constexpr f32 Dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr f32 Dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr f32 Dot(Vec4 a, Vec4 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] constexpr Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr f32 LengthSquared(Vec3 v) noexcept { return Dot(v, v); }
[[nodiscard]] inline f32 Length(Vec3 v) noexcept { return std::sqrt(LengthSquared(v)); }
[[nodiscard]] constexpr f32 LengthSquared(Vec2 v) noexcept { return Dot(v, v); }
[[nodiscard]] inline f32 Length(Vec2 v) noexcept { return std::sqrt(LengthSquared(v)); }

[[nodiscard]] inline f32 Distance(Vec3 a, Vec3 b) noexcept { return Length(a - b); }
[[nodiscard]] constexpr f32 DistanceSquared(Vec3 a, Vec3 b) noexcept { return LengthSquared(a - b); }

/// Normalize, returning the zero vector for degenerate input rather than NaN.
/// Degenerate inputs are a data problem; propagating NaN silently would turn
/// one bad asset into an invisible black screen.
[[nodiscard]] inline Vec3 Normalize(Vec3 v) noexcept {
    const f32 length = Length(v);
    return length > kEpsilon ? v / length : Vec3::Zero();
}

[[nodiscard]] inline Vec2 Normalize(Vec2 v) noexcept {
    const f32 length = Length(v);
    return length > kEpsilon ? v / length : Vec2{0.0f, 0.0f};
}

[[nodiscard]] inline Vec4 Normalize(Vec4 v) noexcept {
    const f32 length = std::sqrt(Dot(v, v));
    return length > kEpsilon ? v / length : Vec4{0.0f, 0.0f, 0.0f, 0.0f};
}

[[nodiscard]] constexpr Vec3 Lerp(Vec3 a, Vec3 b, f32 t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec2 Lerp(Vec2 a, Vec2 b, f32 t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec4 Lerp(Vec4 a, Vec4 b, f32 t) noexcept { return a + (b - a) * t; }

[[nodiscard]] constexpr Vec3 Min(Vec3 a, Vec3 b) noexcept {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
[[nodiscard]] constexpr Vec3 Max(Vec3 a, Vec3 b) noexcept {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
[[nodiscard]] constexpr Vec3 Abs(Vec3 a) noexcept {
    return {a.x < 0.0f ? -a.x : a.x, a.y < 0.0f ? -a.y : a.y, a.z < 0.0f ? -a.z : a.z};
}
[[nodiscard]] constexpr Vec3 Clamp(Vec3 v, Vec3 min, Vec3 max) noexcept {
    return Max(min, Min(v, max));
}

[[nodiscard]] inline bool ApproximatelyEqual(Vec3 a, Vec3 b, f32 epsilon = kEpsilon) noexcept {
    return Approximately(a.x, b.x, epsilon) && Approximately(a.y, b.y, epsilon) &&
           Approximately(a.z, b.z, epsilon);
}
[[nodiscard]] inline bool ApproximatelyEqual(Vec4 a, Vec4 b, f32 epsilon = kEpsilon) noexcept {
    return Approximately(a.x, b.x, epsilon) && Approximately(a.y, b.y, epsilon) &&
           Approximately(a.z, b.z, epsilon) && Approximately(a.w, b.w, epsilon);
}

/// Reflect `incident` around `normal` (normal must be unit length).
[[nodiscard]] constexpr Vec3 Reflect(Vec3 incident, Vec3 normal) noexcept {
    return incident - normal * (2.0f * Dot(incident, normal));
}

/// Component-wise safe divide used by scale transforms.
[[nodiscard]] constexpr Vec3 DivideOrZero(Vec3 a, Vec3 b) noexcept {
    return {b.x != 0.0f ? a.x / b.x : 0.0f, b.y != 0.0f ? a.y / b.y : 0.0f,
            b.z != 0.0f ? a.z / b.z : 0.0f};
}

} // namespace l3d::math
