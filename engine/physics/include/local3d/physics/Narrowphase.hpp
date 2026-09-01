#pragma once
/// @file Narrowphase.hpp
/// @brief Shape pair intersection: pure geometry, no world and no handles.
///
/// Public because it is useful outside the simulation - a debug overlay that
/// draws contacts, an editor snap tool, a custom query - and because the
/// intersection tests are the part of physics most worth testing precisely, and
/// testing them through a stepping world would only test them indirectly.
///
/// One contact point per pair.  A real manifold (up to four points for a box on
/// a box) is what makes a stack of crates perfectly still; a single point is
/// enough for gameplay collisions and a few stacked boxes, and it keeps the
/// solver small.  See docs/architecture/physics.md for what this costs.

#include "local3d/math/Transform.hpp"

#include <array>
#include "local3d/physics/PhysicsTypes.hpp"

namespace l3d::physics {

/// A shape at a pose.  Split out from the body so the intersection tests can be
/// called directly from tests, with no world and no handles involved.
struct ShapePose {
    CollisionShape shape;
    math::Vec3 position = math::Vec3::Zero();
    math::Quaternion rotation = math::Quaternion::Identity();
};

/// One contact point of a manifold.
struct ManifoldPoint {
    /// World space point between the two surfaces.
    math::Vec3 position = math::Vec3::Zero();
    /// Overlap in metres; positive means penetrating.
    f32 penetration = 0.0f;
};

/// The result of one shape pair test.
///
/// Box against box produces up to four points, everything else one.  A single
/// point is not a small optimisation, it is a real limitation: a box resting on
/// its face has friction acting at the contact, half a body height below the
/// centre of mass, and one point gives the solver no way to balance the
/// resulting torque.  The box spins up while it slides and eventually walks off
/// whatever it was resting on.  Four points spanning the overlap region let the
/// solver push back at the corners, which is what a real contact patch does.
struct ContactManifold {
    static constexpr u32 kMaxPoints = 4;

    /// Points from `a` towards `b`, for every point in the manifold.
    math::Vec3 normal = math::Vec3::Up();
    std::array<ManifoldPoint, kMaxPoints> points{};
    u32 pointCount = 0;

    [[nodiscard]] bool IsValid() const noexcept { return pointCount > 0; }

    /// The deepest point.  Summary fields for callers that want one answer -
    /// a character controller pushing itself out of a wall, say.
    [[nodiscard]] const ManifoldPoint& Deepest() const noexcept {
        usize deepest = 0;
        for (usize i = 1; i < pointCount; ++i) {
            if (points[i].penetration > points[deepest].penetration) {
                deepest = i;
            }
        }
        return points[deepest];
    }
};

/// Intersects two shapes.  `pointCount` is zero when they are apart.
///
/// The normal always points from `a` towards `b`, whatever the shape order, and
/// the contact point is on the surface between them.  Both of those are load
/// bearing: the solver pushes `a` along `-normal` and `b` along `+normal`, so an
/// inconsistently oriented normal makes bodies attract instead of separate.
[[nodiscard]] ContactManifold Intersect(const ShapePose& a, const ShapePose& b) noexcept;

/// Closest point on the segment `from`-`to` to `point`.
[[nodiscard]] math::Vec3 ClosestPointOnSegment(math::Vec3 point, math::Vec3 from,
                                              math::Vec3 to) noexcept;

/// Closest pair of points between two segments.  Degenerate (parallel) segments
/// fall back to the midpoint of one against the other rather than dividing by
/// zero.
void ClosestPointsBetweenSegments(math::Vec3 p1, math::Vec3 q1, math::Vec3 p2, math::Vec3 q2,
                                 math::Vec3& outOnFirst, math::Vec3& outOnSecond) noexcept;

/// Closest point on an oriented box's surface or interior to `point`.
[[nodiscard]] math::Vec3 ClosestPointOnBox(math::Vec3 point, math::Vec3 boxPosition,
                                          const math::Quaternion& boxRotation,
                                          math::Vec3 halfExtents) noexcept;

} // namespace l3d::physics
