#pragma once
/// @file Geometry.hpp
/// @brief Bounding volumes, planes, rays and frustum culling primitives.
///
/// These types are the currency of the renderer's culling path, so they are
/// allocation free and branch light on purpose.

#include "local3d/math/Matrix.hpp"

namespace l3d::math {

/// Axis aligned bounding box.
struct Aabb {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    [[nodiscard]] constexpr Vec3 Center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr Vec3 Extents() const noexcept { return (max - min) * 0.5f; }
    [[nodiscard]] constexpr Vec3 Size() const noexcept { return max - min; }
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return max.x >= min.x && max.y >= min.y && max.z >= min.z;
    }

    [[nodiscard]] constexpr Aabb Encapsulating(Vec3 point) const noexcept {
        return {Min(min, point), Max(max, point)};
    }

    [[nodiscard]] constexpr Aabb Merged(const Aabb& other) const noexcept {
        return {Min(min, other.min), Max(max, other.max)};
    }

    [[nodiscard]] constexpr Aabb Expanded(f32 amount) const noexcept {
        const Vec3 offset{amount, amount, amount};
        return {min - offset, max + offset};
    }

    [[nodiscard]] constexpr bool Intersects(const Aabb& other) const noexcept {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y &&
               max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
    }

    [[nodiscard]] constexpr bool Contains(Vec3 point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    /// Conservative world space bounds for a transformed box.  Used for mesh
    /// bounds; it over-approximates when rotated, which is the safe direction
    /// for culling.
    [[nodiscard]] inline Aabb Transformed(const Mat4& matrix) const noexcept {
        const Vec3 center = Center();
        const Vec3 extents = Extents();
        const Vec3 newCenter = matrix.TransformPoint(center);

        // Transform the extents by the absolute value of the basis vectors.
        Vec3 xAxis{matrix.At(0, 0), matrix.At(0, 1), matrix.At(0, 2)};
        Vec3 yAxis{matrix.At(1, 0), matrix.At(1, 1), matrix.At(1, 2)};
        Vec3 zAxis{matrix.At(2, 0), matrix.At(2, 1), matrix.At(2, 2)};
        xAxis = Abs(xAxis) * extents.x;
        yAxis = Abs(yAxis) * extents.y;
        zAxis = Abs(zAxis) * extents.z;
        const Vec3 newExtents = xAxis + yAxis + zAxis;
        return {newCenter - newExtents, newCenter + newExtents};
    }
};

struct Sphere {
    Vec3 center = Vec3::Zero();
    f32 radius = 0.0f;

    [[nodiscard]] constexpr bool Contains(Vec3 point) const noexcept {
        return DistanceSquared(center, point) <= radius * radius;
    }

    [[nodiscard]] constexpr bool Intersects(const Sphere& other) const noexcept {
        const f32 combined = radius + other.radius;
        return DistanceSquared(center, other.center) <= combined * combined;
    }
};

/// Infinite plane: normal . p + distance = 0.  Normal points to the "front".
struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 distance = 0.0f;

    [[nodiscard]] static constexpr Plane FromNormalAndPoint(Vec3 n, Vec3 p) noexcept {
        return {n, Dot(n, p)};
    }

    [[nodiscard]] static inline Plane FromPoints(Vec3 a, Vec3 b, Vec3 c) noexcept {
        const Vec3 n = Normalize(Cross(b - a, c - a));
        return FromNormalAndPoint(n, a);
    }

    [[nodiscard]] constexpr f32 SignedDistance(Vec3 point) const noexcept {
        return Dot(normal, point) - distance;
    }

    [[nodiscard]] constexpr Vec3 ClosestPoint(Vec3 point) const noexcept {
        return point - normal * SignedDistance(point);
    }

    /// Normalize is not constexpr because it needs a square root.
    [[nodiscard]] inline Plane Normalized() const noexcept {
        const f32 length = Length(normal);
        return length > kEpsilon ? Plane{normal / length, distance / length} : *this;
    }
};

struct Ray {
    Vec3 origin = Vec3::Zero();
    Vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] constexpr Vec3 At(f32 t) const noexcept { return origin + direction * t; }

    /// Slab test.  Returns false for a miss; `tMin`/`tMax` give the entry/exit.
    [[nodiscard]] inline bool IntersectAabb(const Aabb& box, f32& tMin, f32& tMax) const noexcept {
        tMin = 0.0f;
        tMax = kInfinity;
        for (usize axis = 0; axis < 3; ++axis) {
            const f32 d = direction[axis];
            const f32 o = origin[axis];
            if (d > -kEpsilon && d < kEpsilon) {
                // Parallel to the slab: outside means no intersection at all.
                if (o < box.min[axis] || o > box.max[axis]) {
                    return false;
                }
                continue;
            }
            const f32 invD = 1.0f / d;
            f32 t1 = (box.min[axis] - o) * invD;
            f32 t2 = (box.max[axis] - o) * invD;
            if (t1 > t2) {
                const f32 swap = t1;
                t1 = t2;
                t2 = swap;
            }
            tMin = t1 > tMin ? t1 : tMin;
            tMax = t2 < tMax ? t2 : tMax;
            if (tMin > tMax) {
                return false;
            }
        }
        return tMax >= 0.0f;
    }

    /// Returns false for a miss.
    [[nodiscard]] inline bool IntersectSphere(const Sphere& sphere, f32& tHit) const noexcept {
        const Vec3 toCenter = sphere.center - origin;
        const f32 projection = Dot(toCenter, direction);
        const f32 distanceSq = LengthSquared(toCenter);
        const f32 radiusSq = sphere.radius * sphere.radius;
        if (distanceSq > radiusSq && projection < 0.0f) {
            return false; // Behind the ray.
        }
        const f32 discriminant = radiusSq - (distanceSq - projection * projection);
        if (discriminant < 0.0f) {
            return false;
        }
        tHit = projection - std::sqrt(discriminant);
        return tHit >= 0.0f;
    }

    /// Returns false when the plane is parallel or behind the ray.
    [[nodiscard]] inline bool IntersectPlane(const Plane& plane, f32& tHit) const noexcept {
        const f32 denominator = Dot(plane.normal, direction);
        if (denominator > -kEpsilon && denominator < kEpsilon) {
            return false;
        }
        tHit = (plane.distance - Dot(plane.normal, origin)) / denominator;
        return tHit >= 0.0f;
    }
};

/// View frustum: six planes, built from a view-projection matrix.
/// Plane order: left, right, bottom, top, near, far.  Normals point inward.
struct Frustum {
    Plane planes[6];

    [[nodiscard]] static inline Frustum FromViewProjection(const Mat4& viewProjection) noexcept {
        Frustum frustum;
        const f32* m = viewProjection.Data(); // column major

        // Gribb/Hartmann plane extraction.
        auto extract = [m](usize rowA, usize rowB, int sign) {
            Plane plane;
            const f32 s = static_cast<f32>(sign);
            plane.normal = Vec3{m[0 * 4 + rowA] + s * m[0 * 4 + rowB],
                                m[1 * 4 + rowA] + s * m[1 * 4 + rowB],
                                m[2 * 4 + rowA] + s * m[2 * 4 + rowB]};
            plane.distance = -(m[3 * 4 + rowA] + s * m[3 * 4 + rowB]);
            return plane.Normalized();
        };

        // Clip space is Vulkan style (z in [0,1]), so the near plane is z >= 0
        // rather than z >= -w.  `sign == 0` selects a single row.
        frustum.planes[0] = extract(0, 3, +1); // left:   x + w >= 0
        frustum.planes[1] = extract(3, 0, -1); // right:  w - x >= 0
        frustum.planes[2] = extract(1, 3, +1); // bottom: y + w >= 0
        frustum.planes[3] = extract(3, 1, -1); // top:    w - y >= 0
        frustum.planes[4] = extract(2, 3, 0);  // near:   z >= 0
        frustum.planes[5] = extract(3, 2, -1); // far:    w - z >= 0
        return frustum;
    }

    enum class Test { Inside, Intersecting, Outside };

    [[nodiscard]] inline Test TestSphere(const Sphere& sphere) const noexcept {
        bool allInside = true;
        for (const Plane& plane : planes) {
            const f32 distance = plane.SignedDistance(sphere.center);
            if (distance < -sphere.radius) {
                return Test::Outside;
            }
            if (distance < sphere.radius) {
                allInside = false;
            }
        }
        return allInside ? Test::Inside : Test::Intersecting;
    }

    [[nodiscard]] inline Test TestAabb(const Aabb& box) const noexcept {
        const Vec3 center = box.Center();
        const Vec3 extents = box.Extents();
        bool allInside = true;
        for (const Plane& plane : planes) {
            // Project the box onto the plane normal: the radius of the box
            // along that direction.
            const f32 projectedRadius = Abs(plane.normal).x * extents.x +
                                        Abs(plane.normal).y * extents.y +
                                        Abs(plane.normal).z * extents.z;
            const f32 distance = plane.SignedDistance(center);
            if (distance < -projectedRadius) {
                return Test::Outside;
            }
            if (distance < projectedRadius) {
                allInside = false;
            }
        }
        return allInside ? Test::Inside : Test::Intersecting;
    }

    [[nodiscard]] inline bool ContainsPoint(Vec3 point) const noexcept {
        for (const Plane& plane : planes) {
            if (plane.SignedDistance(point) < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

} // namespace l3d::math
