#pragma once
/// @file Quaternion.hpp
/// @brief Unit quaternions for rotation.
///
/// Storage order is (x, y, z, w) with w the scalar part, matching GLSL and the
/// engine's shader constant layouts.

#include "local3d/math/Matrix.hpp"
#include "local3d/math/Vector.hpp"

#include <cmath>

namespace l3d::math {

struct Quaternion {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;

    constexpr Quaternion() noexcept = default;
    constexpr Quaternion(f32 inX, f32 inY, f32 inZ, f32 inW) noexcept
        : x(inX), y(inY), z(inZ), w(inW) {}

    [[nodiscard]] static constexpr Quaternion Identity() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    [[nodiscard]] static inline Quaternion FromAxisAngle(Vec3 axis, f32 radians) noexcept {
        const Vec3 unit = Normalize(axis);
        const f32 half = radians * 0.5f;
        const f32 s = std::sin(half);
        return {unit.x * s, unit.y * s, unit.z * s, std::cos(half)};
    }

    /// Euler angles in radians, applied as yaw(Y) * pitch(X) * roll(Z).
    [[nodiscard]] static inline Quaternion FromEuler(f32 pitch, f32 yaw, f32 roll) noexcept {
        const f32 cy = std::cos(yaw * 0.5f);
        const f32 sy = std::sin(yaw * 0.5f);
        const f32 cp = std::cos(pitch * 0.5f);
        const f32 sp = std::sin(pitch * 0.5f);
        const f32 cr = std::cos(roll * 0.5f);
        const f32 sr = std::sin(roll * 0.5f);
        return {
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr,
        };
    }

    [[nodiscard]] static inline Quaternion FromEuler(Vec3 eulerRadians) noexcept {
        return FromEuler(eulerRadians.x, eulerRadians.y, eulerRadians.z);
    }

    /// Orientation whose -Z axis points along `forward` (right handed, Y up).
    [[nodiscard]] static inline Quaternion LookRotation(Vec3 forward, Vec3 up = Vec3::Up()) noexcept;

    [[nodiscard]] constexpr Quaternion Conjugated() const noexcept { return {-x, -y, -z, w}; }

    [[nodiscard]] constexpr f32 LengthSquared() const noexcept {
        return x * x + y * y + z * z + w * w;
    }
    [[nodiscard]] inline f32 Length() const noexcept { return std::sqrt(LengthSquared()); }

    [[nodiscard]] inline Quaternion Normalized() const noexcept {
        const f32 length = Length();
        if (length < kEpsilon) {
            return Identity();
        }
        const f32 inv = 1.0f / length;
        return {x * inv, y * inv, z * inv, w * inv};
    }

    [[nodiscard]] inline Quaternion Inverse() const noexcept {
        const f32 lengthSq = LengthSquared();
        if (lengthSq < kEpsilon) {
            return Identity();
        }
        const f32 inv = 1.0f / lengthSq;
        return {-x * inv, -y * inv, -z * inv, w * inv};
    }

    /// Hamilton product: applying `b` first, then `a`.
    friend inline Quaternion operator*(const Quaternion& a, const Quaternion& b) noexcept {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }

    /// Rotate a vector by this quaternion.
    [[nodiscard]] inline Vec3 Rotate(Vec3 v) const noexcept {
        // q * v * q^-1, expanded to avoid building the intermediate quaternions.
        const Vec3 qv{x, y, z};
        const Vec3 t = 2.0f * Cross(qv, v);
        return v + w * t + Cross(qv, t);
    }

    [[nodiscard]] inline Mat3 ToMat3() const noexcept {
        const f32 xx = x * x;
        const f32 yy = y * y;
        const f32 zz = z * z;
        const f32 xy = x * y;
        const f32 xz = x * z;
        const f32 yz = y * z;
        const f32 wx = w * x;
        const f32 wy = w * y;
        const f32 wz = w * z;

        Mat3 result;
        result.Set(0, 0, 1.0f - 2.0f * (yy + zz));
        result.Set(0, 1, 2.0f * (xy - wz));
        result.Set(0, 2, 2.0f * (xz + wy));
        result.Set(1, 0, 2.0f * (xy + wz));
        result.Set(1, 1, 1.0f - 2.0f * (xx + zz));
        result.Set(1, 2, 2.0f * (yz - wx));
        result.Set(2, 0, 2.0f * (xz - wy));
        result.Set(2, 1, 2.0f * (yz + wx));
        result.Set(2, 2, 1.0f - 2.0f * (xx + yy));
        return result;
    }

    [[nodiscard]] inline Mat4 ToMat4() const noexcept {
        const Mat3 rotation = ToMat3();
        Mat4 result = Mat4::Identity();
        for (usize c = 0; c < 3; ++c) {
            for (usize r = 0; r < 3; ++r) {
                result.Set(r, c, rotation.At(r, c));
            }
        }
        return result;
    }

    /// Rebuild a quaternion from a (possibly scaled) rotation matrix.
    [[nodiscard]] static inline Quaternion FromMat3(const Mat3& matrix) noexcept;

    /// Extract euler angles (pitch, yaw, roll) in radians.  Round trips with
    /// FromEuler away from gimbal lock.
    [[nodiscard]] inline Vec3 ToEuler() const noexcept;

    /// Normalised linear interpolation: cheap, constant angular speed is only
    /// approximate, but visually identical for small angles (animation, camera).
    [[nodiscard]] static inline Quaternion Nlerp(const Quaternion& a, const Quaternion& b,
                                                 f32 t) noexcept;

    /// Spherical interpolation for large angular distances.
    [[nodiscard]] static inline Quaternion Slerp(const Quaternion& a, const Quaternion& b,
                                                 f32 t) noexcept;

    [[nodiscard]] static constexpr f32 Dot(const Quaternion& a, const Quaternion& b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    friend constexpr bool operator==(const Quaternion& a, const Quaternion& b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
};

// --- Out of line definitions that need the full matrix type ----------------

inline Quaternion Quaternion::LookRotation(Vec3 forward, Vec3 up) noexcept {
    const Vec3 zAxis = Normalize(-forward); // Right handed: -Z is forward.
    Vec3 xAxis = Normalize(Cross(up, zAxis));
    if (math::LengthSquared(xAxis) < kEpsilon) {
        // `up` is parallel to `forward`; pick any perpendicular axis.
        xAxis = Normalize(Cross(Vec3::Right(), zAxis));
    }
    const Vec3 yAxis = Cross(zAxis, xAxis);

    Mat3 matrix;
    matrix.Set(0, 0, xAxis.x);
    matrix.Set(1, 0, xAxis.y);
    matrix.Set(2, 0, xAxis.z);
    matrix.Set(0, 1, yAxis.x);
    matrix.Set(1, 1, yAxis.y);
    matrix.Set(2, 1, yAxis.z);
    matrix.Set(0, 2, zAxis.x);
    matrix.Set(1, 2, zAxis.y);
    matrix.Set(2, 2, zAxis.z);
    return FromMat3(matrix);
}

inline Quaternion Quaternion::FromMat3(const Mat3& matrix) noexcept {
    // Shepperd's method: pick the largest diagonal term for numerical stability.
    const f32 m00 = matrix.At(0, 0);
    const f32 m11 = matrix.At(1, 1);
    const f32 m22 = matrix.At(2, 2);
    const f32 trace = m00 + m11 + m22;
    Quaternion result;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f; // s = 4w
        result.w = 0.25f * s;
        result.x = (matrix.At(2, 1) - matrix.At(1, 2)) / s;
        result.y = (matrix.At(0, 2) - matrix.At(2, 0)) / s;
        result.z = (matrix.At(1, 0) - matrix.At(0, 1)) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f; // s = 4x
        result.w = (matrix.At(2, 1) - matrix.At(1, 2)) / s;
        result.x = 0.25f * s;
        result.y = (matrix.At(0, 1) + matrix.At(1, 0)) / s;
        result.z = (matrix.At(0, 2) + matrix.At(2, 0)) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f; // s = 4y
        result.w = (matrix.At(0, 2) - matrix.At(2, 0)) / s;
        result.x = (matrix.At(0, 1) + matrix.At(1, 0)) / s;
        result.y = 0.25f * s;
        result.z = (matrix.At(1, 2) + matrix.At(2, 1)) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f; // s = 4z
        result.w = (matrix.At(1, 0) - matrix.At(0, 1)) / s;
        result.x = (matrix.At(0, 2) + matrix.At(2, 0)) / s;
        result.y = (matrix.At(1, 2) + matrix.At(2, 1)) / s;
        result.z = 0.25f * s;
    }
    return result.Normalized();
}

inline Vec3 Quaternion::ToEuler() const noexcept {
    // Yaw (Y), pitch (X), roll (Z) matching FromEuler's order.
    const f32 sinPitch = 2.0f * (w * x - y * z);
    f32 pitch = 0.0f;
    f32 yaw = 0.0f;
    f32 roll = 0.0f;

    if (sinPitch >= 1.0f - kEpsilon) {
        pitch = kHalfPi; // Gimbal lock.
        yaw = std::atan2(2.0f * (x * y + w * z), 1.0f - 2.0f * (y * y + z * z));
    } else if (sinPitch <= -1.0f + kEpsilon) {
        pitch = -kHalfPi;
        yaw = std::atan2(2.0f * (x * y + w * z), 1.0f - 2.0f * (y * y + z * z));
    } else {
        pitch = std::asin(Clamp(sinPitch, -1.0f, 1.0f));
        yaw = std::atan2(2.0f * (x * z + w * y), 1.0f - 2.0f * (x * x + y * y));
        roll = std::atan2(2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z));
    }
    return {pitch, yaw, roll};
}

inline Quaternion Quaternion::Nlerp(const Quaternion& a, const Quaternion& b, f32 t) noexcept {
    // Take the short path around the hypersphere.
    Quaternion target = b;
    if (Dot(a, b) < 0.0f) {
        target = {-b.x, -b.y, -b.z, -b.w};
    }
    return Quaternion{Lerp(a.x, target.x, t), Lerp(a.y, target.y, t), Lerp(a.z, target.z, t),
                      Lerp(a.w, target.w, t)}
        .Normalized();
}

inline Quaternion Quaternion::Slerp(const Quaternion& a, const Quaternion& b, f32 t) noexcept {
    Quaternion target = b;
    f32 cosTheta = Dot(a, b);
    if (cosTheta < 0.0f) {
        target = {-b.x, -b.y, -b.z, -b.w};
        cosTheta = -cosTheta;
    }
    if (cosTheta > 1.0f - kEpsilon) {
        return Nlerp(a, target, t); // Nearly identical: avoid dividing by zero.
    }
    const f32 theta = std::acos(Clamp(cosTheta, -1.0f, 1.0f));
    const f32 sinTheta = std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) / sinTheta;
    const f32 wb = std::sin(t * theta) / sinTheta;
    return Quaternion{wa * a.x + wb * target.x, wa * a.y + wb * target.y,
                      wa * a.z + wb * target.z, wa * a.w + wb * target.w};
}

inline Mat4 Mat4::Inverse() const noexcept {
    // Cofactor expansion.  Written out explicitly because this is a hot path
    // (once per camera per frame) and clarity beats cleverness here.
    const f32* s = &m[0][0]; // column major
    f32 inv[16];

    inv[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] +
             s[9] * s[7] * s[14] + s[13] * s[6] * s[11] - s[13] * s[7] * s[10];
    inv[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] -
             s[8] * s[7] * s[14] - s[12] * s[6] * s[11] + s[12] * s[7] * s[10];
    inv[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] +
             s[8] * s[7] * s[13] + s[12] * s[5] * s[11] - s[12] * s[7] * s[9];
    inv[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] -
              s[8] * s[6] * s[13] - s[12] * s[5] * s[10] + s[12] * s[6] * s[9];
    inv[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] -
             s[9] * s[3] * s[14] - s[13] * s[2] * s[11] + s[13] * s[3] * s[10];
    inv[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] +
             s[8] * s[3] * s[14] + s[12] * s[2] * s[11] - s[12] * s[3] * s[10];
    inv[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] -
             s[8] * s[3] * s[13] - s[12] * s[1] * s[11] + s[12] * s[3] * s[9];
    inv[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] +
              s[8] * s[2] * s[13] + s[12] * s[1] * s[10] - s[12] * s[2] * s[9];
    inv[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] +
             s[5] * s[3] * s[14] + s[13] * s[2] * s[7] - s[13] * s[3] * s[6];
    inv[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] -
             s[4] * s[3] * s[14] - s[12] * s[2] * s[7] + s[12] * s[3] * s[6];
    inv[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] +
              s[4] * s[3] * s[13] + s[12] * s[1] * s[7] - s[12] * s[3] * s[5];
    inv[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] -
              s[4] * s[2] * s[13] - s[12] * s[1] * s[6] + s[12] * s[2] * s[5];
    inv[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] -
             s[5] * s[3] * s[10] - s[9] * s[2] * s[7] + s[9] * s[3] * s[6];
    inv[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] +
             s[4] * s[3] * s[10] + s[8] * s[2] * s[7] - s[8] * s[3] * s[6];
    inv[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] -
              s[4] * s[3] * s[9] - s[8] * s[1] * s[7] + s[8] * s[3] * s[5];
    inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] +
              s[4] * s[2] * s[9] + s[8] * s[1] * s[6] - s[8] * s[2] * s[5];

    const f32 det = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
    if (det == 0.0f || !std::isfinite(det)) {
        return Identity();
    }
    const f32 invDet = 1.0f / det;
    Mat4 result;
    for (usize i = 0; i < 16; ++i) {
        (&result.m[0][0])[i] = inv[i] * invDet;
    }
    return result;
}

inline bool Mat4::Decompose(Vec3& outTranslation, Quaternion& outRotation,
                            Vec3& outScale) const noexcept {
    outTranslation = {m[3][0], m[3][1], m[3][2]};

    Vec3 column0{m[0][0], m[0][1], m[0][2]};
    const Vec3 column1{m[1][0], m[1][1], m[1][2]};
    const Vec3 column2{m[2][0], m[2][1], m[2][2]};

    outScale = {Length(column0), Length(column1), Length(column2)};
    if (outScale.x < kEpsilon || outScale.y < kEpsilon || outScale.z < kEpsilon) {
        outRotation = Quaternion::Identity();
        return false;
    }

    // A negative determinant means the basis is mirrored; encode it in X.
    if (Dot(Cross(column0, column1), column2) < 0.0f) {
        outScale.x = -outScale.x;
    }

    column0 = column0 / outScale.x;
    const Vec3 axis1 = column1 / outScale.y;
    const Vec3 axis2 = column2 / outScale.z;

    Mat3 rotation;
    rotation.Set(0, 0, column0.x);
    rotation.Set(1, 0, column0.y);
    rotation.Set(2, 0, column0.z);
    rotation.Set(0, 1, axis1.x);
    rotation.Set(1, 1, axis1.y);
    rotation.Set(2, 1, axis1.z);
    rotation.Set(0, 2, axis2.x);
    rotation.Set(1, 2, axis2.y);
    rotation.Set(2, 2, axis2.z);

    outRotation = Quaternion::FromMat3(rotation);
    return true;
}

inline Mat4 Mat4::Compose(Vec3 translation, const Quaternion& rotation, Vec3 scale) noexcept {
    return Translation(translation) * rotation.ToMat4() * Scaling(scale);
}

} // namespace l3d::math
