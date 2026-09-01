#include "local3d/physics/PhysicsTypes.hpp"

#include "local3d/math/Constants.hpp"

#include <cmath>

namespace l3d::physics {

f32 CollisionShape::Volume() const noexcept {
    switch (type) {
        case ShapeType::Sphere:
            return (4.0f / 3.0f) * math::kPi * radius * radius * radius;
        case ShapeType::Box:
            // Full edge lengths are twice the half extents.
            return 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
        case ShapeType::Capsule: {
            const f32 cylinder = math::kPi * radius * radius * (2.0f * halfLength);
            const f32 caps = (4.0f / 3.0f) * math::kPi * radius * radius * radius;
            return cylinder + caps;
        }
    }
    return 0.0f;
}

math::Aabb CollisionShape::LocalBounds() const noexcept {
    math::Aabb bounds;
    switch (type) {
        case ShapeType::Sphere:
            bounds.min = math::Vec3{-radius, -radius, -radius};
            bounds.max = math::Vec3{radius, radius, radius};
            break;
        case ShapeType::Box:
            bounds.min = -halfExtents;
            bounds.max = halfExtents;
            break;
        case ShapeType::Capsule: {
            const f32 height = halfLength + radius;
            bounds.min = math::Vec3{-radius, -height, -radius};
            bounds.max = math::Vec3{radius, height, radius};
            break;
        }
    }
    return bounds;
}

math::Aabb CollisionShape::BoundsAt(const math::Transform& pose) const noexcept {
    const math::Aabb local = LocalBounds();
    const math::Vec3 half = local.Extents();

    // The world half extent along an axis is the sum of the projected basis
    // vectors: |R * half|.  Cheaper and more accurate than transforming the
    // eight corners and encapsulating.
    const math::Vec3 xAxis = pose.rotation.Rotate(math::Vec3{half.x, 0.0f, 0.0f});
    const math::Vec3 yAxis = pose.rotation.Rotate(math::Vec3{0.0f, half.y, 0.0f});
    const math::Vec3 zAxis = pose.rotation.Rotate(math::Vec3{0.0f, 0.0f, half.z});
    const math::Vec3 worldHalf = math::Abs(xAxis) + math::Abs(yAxis) + math::Abs(zAxis);

    // Every shape here is symmetric about its own origin, so the local centre is
    // zero and the world centre is the pose position.
    const math::Vec3 center = pose.TransformPoint(math::Vec3::Zero());
    math::Aabb bounds;
    bounds.min = center - worldHalf;
    bounds.max = center + worldHalf;
    return bounds;
}

math::Vec3 CollisionShape::UnitInertia() const noexcept {
    switch (type) {
        case ShapeType::Sphere: {
            // Solid sphere: 2/5 m r^2 on every axis.
            const f32 diagonal = 0.4f * radius * radius;
            return math::Vec3{diagonal, diagonal, diagonal};
        }
        case ShapeType::Box: {
            // Solid box: m/12 (h^2 + d^2) per axis, with h and d the full edges.
            const f32 x = halfExtents.x * 2.0f;
            const f32 y = halfExtents.y * 2.0f;
            const f32 z = halfExtents.z * 2.0f;
            return math::Vec3{(y * y + z * z) / 12.0f, (x * x + z * z) / 12.0f,
                              (x * x + y * y) / 12.0f};
        }
        case ShapeType::Capsule: {
            // A cylinder plus two hemispheres, mass weighted by volume, with the
            // hemispheres moved out to their own centroids (3r/8 from the centre
            // of the sphere they form) through the parallel axis theorem.
            const f32 cylinderLength = 2.0f * halfLength;
            const f32 cylinderVolume = math::kPi * radius * radius * cylinderLength;
            const f32 sphereVolume = (4.0f / 3.0f) * math::kPi * radius * radius * radius;
            const f32 total = cylinderVolume + sphereVolume;
            if (total <= 0.0f) {
                return math::Vec3::One();
            }
            const f32 cylinderFraction = cylinderVolume / total;
            const f32 sphereFraction = sphereVolume / total;
            const f32 offset = cylinderLength * 0.5f + radius * 0.375f;

            const f32 aroundAxis = cylinderFraction * (0.5f * radius * radius) +
                                   sphereFraction * (0.4f * radius * radius);
            const f32 acrossAxis =
                cylinderFraction * (radius * radius * 0.25f + cylinderLength * cylinderLength / 12.0f) +
                sphereFraction * (0.4f * radius * radius + offset * offset);
            return math::Vec3{acrossAxis, aroundAxis, acrossAxis};
        }
    }
    return math::Vec3::One();
}

bool RaycastSettings::Accepts(BodyType type, bool isTrigger) const noexcept {
    if (isTrigger && !hitTriggers) {
        return false;
    }
    switch (type) {
        case BodyType::Static: return hitStatic;
        case BodyType::Kinematic: return hitKinematic;
        case BodyType::Dynamic: return hitDynamic;
    }
    return false;
}

} // namespace l3d::physics
