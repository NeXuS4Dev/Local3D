#pragma once
/// @file Transform.hpp
/// @brief Position/rotation/scale triple - the language of the scene graph.

#include "local3d/math/Quaternion.hpp"

namespace l3d::math {

/// A local transform.  Scene nodes store this plus a parent link; the Scene
/// module computes world transforms from the hierarchy.
struct Transform {
    Vec3 position = Vec3::Zero();
    Quaternion rotation = Quaternion::Identity();
    Vec3 scale = Vec3::One();

    [[nodiscard]] constexpr bool IsIdentity() const noexcept {
        return position == Vec3::Zero() && rotation == Quaternion::Identity() &&
               scale == Vec3::One();
    }

    [[nodiscard]] Mat4 ToMatrix() const noexcept { return Mat4::Compose(position, rotation, scale); }

    [[nodiscard]] Vec3 TransformPoint(Vec3 local) const noexcept {
        return position + rotation.Rotate(local * scale);
    }

    /// Transform a direction: rotation only (scale would skew a unit vector).
    [[nodiscard]] Vec3 TransformDirection(Vec3 localDirection) const noexcept {
        return rotation.Rotate(localDirection);
    }

    [[nodiscard]] Vec3 InverseTransformPoint(Vec3 world) const noexcept {
        return rotation.Inverse().Rotate(world - position) / SafeScale(scale);
    }

    [[nodiscard]] Vec3 Forward() const noexcept { return rotation.Rotate(Vec3::Forward()); }
    [[nodiscard]] Vec3 Up() const noexcept { return rotation.Rotate(Vec3::Up()); }
    [[nodiscard]] Vec3 Right() const noexcept { return rotation.Rotate(Vec3::Right()); }

    /// Orientation looking at `target` from this transform's position.
    [[nodiscard]] static Transform LookAt(Vec3 eye, Vec3 target, Vec3 up = Vec3::Up()) noexcept {
        Transform result;
        result.position = eye;
        result.rotation = Quaternion::LookRotation(target - eye, up);
        return result;
    }

    /// Interpolate two transforms (used by animation and network smoothing).
    [[nodiscard]] static Transform Lerp(const Transform& a, const Transform& b, f32 t) noexcept {
        Transform result;
        result.position = math::Lerp(a.position, b.position, t);
        result.rotation = Quaternion::Slerp(a.rotation, b.rotation, t);
        result.scale = math::Lerp(a.scale, b.scale, t);
        return result;
    }

    /// Compose a parent with a child: parent * child.
    [[nodiscard]] static Transform Combine(const Transform& parent,
                                           const Transform& child) noexcept {
        Transform result;
        result.position = parent.TransformPoint(child.position);
        result.rotation = (parent.rotation * child.rotation).Normalized();
        result.scale = parent.scale * child.scale;
        return result;
    }

    /// Inverse of a rigid transform (scale 1) - used for view matrices.
    [[nodiscard]] Transform InverseRigid() const noexcept {
        Transform result;
        result.rotation = rotation.Inverse();
        result.position = result.rotation.Rotate(-position);
        result.scale = Vec3::One();
        return result;
    }

private:
    /// Component-wise divide that tolerates zero scale on an axis.
    [[nodiscard]] static Vec3 SafeScale(Vec3 s) noexcept {
        return {s.x == 0.0f ? 1.0f : s.x, s.y == 0.0f ? 1.0f : s.y, s.z == 0.0f ? 1.0f : s.z};
    }
};

} // namespace l3d::math
