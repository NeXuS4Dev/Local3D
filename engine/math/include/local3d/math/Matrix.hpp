#pragma once
/// @file Matrix.hpp
/// @brief Mat3 and Mat4.
///
/// Storage is column major (`m[column][row]`), matching GLSL and Vulkan shader
/// layout so matrices upload to the GPU without transposition.

#include "local3d/core/Assert.hpp"
#include "local3d/math/Vector.hpp"

namespace l3d::math {

struct Quaternion;

/// 3x3 matrix, used for normal transforms.
struct Mat3 {
    /// m[column][row]
    f32 m[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

    constexpr Mat3() noexcept = default;

    [[nodiscard]] constexpr f32 At(usize row, usize col) const noexcept { return m[col][row]; }
    constexpr void Set(usize row, usize col, f32 value) noexcept { m[col][row] = value; }
    [[nodiscard]] constexpr const f32* Data() const noexcept { return &m[0][0]; }

    [[nodiscard]] static constexpr Mat3 Identity() noexcept { return Mat3{}; }

    [[nodiscard]] static constexpr Mat3 Scale(Vec3 s) noexcept {
        Mat3 result;
        result.m[0][0] = s.x;
        result.m[1][1] = s.y;
        result.m[2][2] = s.z;
        return result;
    }

    [[nodiscard]] constexpr Mat3 Transposed() const noexcept {
        Mat3 result;
        for (usize c = 0; c < 3; ++c) {
            for (usize r = 0; r < 3; ++r) {
                result.m[c][r] = m[r][c];
            }
        }
        return result;
    }

    friend constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
        Mat3 result;
        for (usize c = 0; c < 3; ++c) {
            for (usize r = 0; r < 3; ++r) {
                f32 sum = 0.0f;
                for (usize k = 0; k < 3; ++k) {
                    sum += a.m[k][r] * b.m[c][k];
                }
                result.m[c][r] = sum;
            }
        }
        return result;
    }

    [[nodiscard]] constexpr Vec3 operator*(Vec3 v) const noexcept {
        return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
                m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
                m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
    }
};

/// 4x4 matrix for transforms and view/projection.
struct Mat4 {
    /// m[column][row]
    f32 m[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                   {0.0f, 1.0f, 0.0f, 0.0f},
                   {0.0f, 0.0f, 1.0f, 0.0f},
                   {0.0f, 0.0f, 0.0f, 1.0f}};

    constexpr Mat4() noexcept = default;

    [[nodiscard]] constexpr f32 At(usize row, usize col) const noexcept { return m[col][row]; }
    constexpr void Set(usize row, usize col, f32 value) noexcept { m[col][row] = value; }
    [[nodiscard]] constexpr const f32* Data() const noexcept { return &m[0][0]; }

    [[nodiscard]] static constexpr Mat4 Identity() noexcept { return Mat4{}; }

    [[nodiscard]] static constexpr Mat4 Zero() noexcept {
        Mat4 result;
        for (usize c = 0; c < 4; ++c) {
            for (usize r = 0; r < 4; ++r) {
                result.m[c][r] = 0.0f;
            }
        }
        return result;
    }

    [[nodiscard]] static constexpr Mat4 Translation(Vec3 t) noexcept {
        Mat4 result;
        result.m[3][0] = t.x;
        result.m[3][1] = t.y;
        result.m[3][2] = t.z;
        return result;
    }

    [[nodiscard]] static constexpr Mat4 Scaling(Vec3 s) noexcept {
        Mat4 result;
        result.m[0][0] = s.x;
        result.m[1][1] = s.y;
        result.m[2][2] = s.z;
        return result;
    }

    [[nodiscard]] static constexpr Mat4 RotationX(f32 radians) noexcept {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        Mat4 result;
        result.m[1][1] = c;
        result.m[1][2] = s;
        result.m[2][1] = -s;
        result.m[2][2] = c;
        return result;
    }

    [[nodiscard]] static constexpr Mat4 RotationY(f32 radians) noexcept {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        Mat4 result;
        result.m[0][0] = c;
        result.m[0][2] = -s;
        result.m[2][0] = s;
        result.m[2][2] = c;
        return result;
    }

    [[nodiscard]] static constexpr Mat4 RotationZ(f32 radians) noexcept {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        Mat4 result;
        result.m[0][0] = c;
        result.m[0][1] = s;
        result.m[1][0] = -s;
        result.m[1][1] = c;
        return result;
    }

    /// Right handed perspective with depth in [0, 1] (Vulkan/D3D clip space).
    /// `fovYDegrees` is the *vertical* field of view.
    [[nodiscard]] static inline Mat4 PerspectiveRH(f32 fovYDegrees, f32 aspect, f32 nearPlane,
                                                   f32 farPlane) noexcept {
        const f32 f = 1.0f / std::tan(fovYDegrees * kDegToRad * 0.5f);
        const f32 invRange = 1.0f / (nearPlane - farPlane);
        Mat4 result = Zero();
        result.m[0][0] = f / aspect;
        result.m[1][1] = f;
        result.m[2][2] = farPlane * invRange;
        result.m[2][3] = -1.0f;
        result.m[3][2] = nearPlane * farPlane * invRange;
        return result;
    }

    /// Right handed orthographic projection, depth in [0, 1].
    [[nodiscard]] static constexpr Mat4 OrthographicRH(f32 left, f32 right, f32 bottom, f32 top,
                                                       f32 nearPlane, f32 farPlane) noexcept {
        Mat4 result = Zero();
        result.m[0][0] = 2.0f / (right - left);
        result.m[1][1] = 2.0f / (top - bottom);
        result.m[2][2] = 1.0f / (nearPlane - farPlane);
        result.m[3][0] = (left + right) / (left - right);
        result.m[3][1] = (bottom + top) / (bottom - top);
        result.m[3][2] = nearPlane / (nearPlane - farPlane);
        result.m[3][3] = 1.0f;
        return result;
    }

    /// Right handed view matrix looking from `eye` at `target`.
    [[nodiscard]] static inline Mat4 LookAtRH(Vec3 eye, Vec3 target, Vec3 up) noexcept {
        const Vec3 forward = Normalize(eye - target);
        const Vec3 right = Normalize(Cross(up, forward));
        const Vec3 trueUp = Cross(forward, right);

        Mat4 result = Identity();
        result.m[0][0] = right.x;
        result.m[1][0] = right.y;
        result.m[2][0] = right.z;
        result.m[0][1] = trueUp.x;
        result.m[1][1] = trueUp.y;
        result.m[2][1] = trueUp.z;
        result.m[0][2] = forward.x;
        result.m[1][2] = forward.y;
        result.m[2][2] = forward.z;
        result.m[3][0] = -Dot(right, eye);
        result.m[3][1] = -Dot(trueUp, eye);
        result.m[3][2] = -Dot(forward, eye);
        return result;
    }

    [[nodiscard]] constexpr Mat4 Transposed() const noexcept {
        Mat4 result;
        for (usize c = 0; c < 4; ++c) {
            for (usize r = 0; r < 4; ++r) {
                result.m[c][r] = m[r][c];
            }
        }
        return result;
    }

    friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
        Mat4 result = Zero();
        for (usize c = 0; c < 4; ++c) {
            for (usize r = 0; r < 4; ++r) {
                f32 sum = 0.0f;
                for (usize k = 0; k < 4; ++k) {
                    sum += a.m[k][r] * b.m[c][k];
                }
                result.m[c][r] = sum;
            }
        }
        return result;
    }

    friend constexpr Vec4 operator*(const Mat4& a, Vec4 v) noexcept {
        return {a.m[0][0] * v.x + a.m[1][0] * v.y + a.m[2][0] * v.z + a.m[3][0] * v.w,
                a.m[0][1] * v.x + a.m[1][1] * v.y + a.m[2][1] * v.z + a.m[3][1] * v.w,
                a.m[0][2] * v.x + a.m[1][2] * v.y + a.m[2][2] * v.z + a.m[3][2] * v.w,
                a.m[0][3] * v.x + a.m[1][3] * v.y + a.m[2][3] * v.z + a.m[3][3] * v.w};
    }

    /// Transform a position (w = 1), applying the perspective divide.
    [[nodiscard]] constexpr Vec3 TransformPoint(Vec3 p) const noexcept {
        const Vec4 transformed = *this * Vec4{p, 1.0f};
        if (transformed.w != 0.0f && transformed.w != 1.0f) {
            return transformed.Xyz() / transformed.w;
        }
        return transformed.Xyz();
    }

    /// Transform a direction or normal (w = 0, no translation).
    [[nodiscard]] constexpr Vec3 TransformDirection(Vec3 d) const noexcept {
        return (*this * Vec4{d, 0.0f}).Xyz();
    }

    /// Full 4x4 inverse via cofactors.  Returns identity for singular matrices
    /// so a bad transform degrades visibly instead of producing NaNs.
    [[nodiscard]] inline Mat4 Inverse() const noexcept;

    /// Split a TRS matrix.  Returns false if the matrix is not decomposable
    /// (for example if it contains shear or a reflection).
    [[nodiscard]] inline bool Decompose(Vec3& outTranslation, Quaternion& outRotation,
                                        Vec3& outScale) const noexcept;

    /// Compose translation * rotation * scale.
    [[nodiscard]] static inline Mat4 Compose(Vec3 translation, const Quaternion& rotation,
                                             Vec3 scale) noexcept;

    friend constexpr bool operator==(const Mat4& a, const Mat4& b) noexcept {
        for (usize c = 0; c < 4; ++c) {
            for (usize r = 0; r < 4; ++r) {
                if (a.m[c][r] != b.m[c][r]) {
                    return false;
                }
            }
        }
        return true;
    }
};

[[nodiscard]] inline bool ApproximatelyEqual(const Mat4& a, const Mat4& b,
                                             f32 epsilon = 1e-4f) noexcept {
    for (usize c = 0; c < 4; ++c) {
        for (usize r = 0; r < 4; ++r) {
            if (!Approximately(a.m[c][r], b.m[c][r], epsilon)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace l3d::math
