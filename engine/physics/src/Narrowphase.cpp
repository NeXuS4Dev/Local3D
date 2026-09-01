#include "local3d/physics/Narrowphase.hpp"

#include "local3d/math/Constants.hpp"

#include <array>
#include <cmath>

namespace l3d::physics {
namespace {

/// Below this, a squared length is treated as zero.  Distances in a physics
/// world are metres, so 1e-12 m^2 is a nanometre.
constexpr f32 kTolerance = 1e-12f;

/// Shapes this close still count as touching.
///
/// A body at rest has its penetration oscillating around zero, so without a
/// margin the contact appears and disappears from step to step, and every
/// disappearance is an End event - gameplay watching a trigger zone sees an
/// object leave and re-enter it every few frames while it sits perfectly still.
/// The depth is clamped at zero, so a speculative contact never pushes.
constexpr f32 kContactMargin = 0.02f;

[[nodiscard]] constexpr f32 ClampScalar(f32 value, f32 min, f32 max) noexcept {
    return value < min ? min : (value > max ? max : value);
}

[[nodiscard]] math::Vec3 ToLocal(const ShapePose& pose, math::Vec3 world) noexcept {
    return pose.rotation.Inverse().Rotate(world - pose.position);
}

[[nodiscard]] math::Vec3 ToWorld(const ShapePose& pose, math::Vec3 local) noexcept {
    return pose.position + pose.rotation.Rotate(local);
}

/// The two cap centres of a capsule, whose axis is the body's local +Y.
void CapsuleSegment(const ShapePose& pose, math::Vec3& outFrom, math::Vec3& outTo) noexcept {
    const math::Vec3 axis = pose.rotation.Rotate(math::Vec3::Up()) * pose.shape.halfLength;
    outFrom = pose.position - axis;
    outTo = pose.position + axis;
}

/// Reinterprets a capsule as the sphere at one point on its axis, which is how
/// every capsule pair is reduced to a sphere pair.
[[nodiscard]] ShapePose SphereAt(const ShapePose& capsule, math::Vec3 position) noexcept {
    ShapePose sphere = capsule;
    sphere.shape = CollisionShape::MakeSphere(capsule.shape.radius);
    sphere.position = position;
    return sphere;
}

/// Builds a one point manifold, which is all the round shapes ever produce.
[[nodiscard]] ContactManifold SinglePoint(math::Vec3 normal, math::Vec3 position,
                                         f32 penetration) noexcept {
    ContactManifold manifold;
    manifold.normal = normal;
    manifold.points[0] = ManifoldPoint{position, penetration};
    manifold.pointCount = 1;
    return manifold;
}

[[nodiscard]] ContactManifold SphereVsSphere(const ShapePose& a, const ShapePose& b) noexcept {
    const math::Vec3 delta = b.position - a.position;
    const f32 radiusSum = a.shape.radius + b.shape.radius;
    const f32 distanceSquared = math::LengthSquared(delta);
    const f32 reach = radiusSum + kContactMargin;
    if (distanceSquared >= reach * reach) {
        return ContactManifold{};
    }

    const f32 distance = std::sqrt(distanceSquared);
    if (distanceSquared > kTolerance) {
        const math::Vec3 normal = delta / distance;
        const f32 penetration = radiusSum - distance > 0.0f ? radiusSum - distance : 0.0f;
        // Half way through the overlap, so both shapes get an equal share of the
        // contact point rather than one of them owning it.
        return SinglePoint(normal, a.position + normal * (a.shape.radius - penetration * 0.5f),
                           penetration);
    }

    // Exactly concentric: every direction separates them equally, so pick one
    // that does not depend on floating point noise.
    return SinglePoint(math::Vec3::Up(), a.position, radiusSum);
}

/// Normal points from the box towards the sphere.
[[nodiscard]] ContactManifold BoxVsSphere(const ShapePose& box, const ShapePose& sphere) noexcept {
    const math::Vec3 local = ToLocal(box, sphere.position);
    const math::Vec3 clamped = math::Clamp(local, -box.shape.halfExtents, box.shape.halfExtents);
    const math::Vec3 delta = local - clamped;
    const f32 distanceSquared = math::LengthSquared(delta);
    const f32 radius = sphere.shape.radius;
    // The margin widens what counts as touching, never how deep a touch is.
    const f32 reach = radius + kContactMargin;
    if (distanceSquared > reach * reach) {
        return ContactManifold{};
    }

    if (distanceSquared > kTolerance) {
        const f32 distance = std::sqrt(distanceSquared);
        const f32 penetration = radius - distance > 0.0f ? radius - distance : 0.0f;
        return SinglePoint(box.rotation.Rotate(delta / distance), ToWorld(box, clamped),
                           penetration);
    }

    // The sphere centre is inside the box, so there is no closest surface point.
    // Escape along the axis with the least material in front of it, which is the
    // shortest way out and the direction that stops the body tunnelling through
    // the thin side of a wall.
    f32 least = math::kInfinity;
    usize axis = 0;
    for (usize i = 0; i < 3; ++i) {
        const f32 remaining = box.shape.halfExtents[i] - std::abs(local[i]);
        if (remaining < least) {
            least = remaining;
            axis = i;
        }
    }
    math::Vec3 localNormal = math::Vec3::Zero();
    localNormal[axis] = local[axis] < 0.0f ? -1.0f : 1.0f;
    const math::Vec3 normal = box.rotation.Rotate(localNormal);
    return SinglePoint(normal, sphere.position - normal * radius, radius + least);
}

/// Normal points from the capsule towards the sphere.
[[nodiscard]] ContactManifold CapsuleVsSphere(const ShapePose& capsule,
                                             const ShapePose& sphere) noexcept {
    math::Vec3 from = math::Vec3::Zero();
    math::Vec3 to = math::Vec3::Zero();
    CapsuleSegment(capsule, from, to);
    return SphereVsSphere(SphereAt(capsule, ClosestPointOnSegment(sphere.position, from, to)),
                          sphere);
}

[[nodiscard]] ContactManifold CapsuleVsCapsule(const ShapePose& a, const ShapePose& b) noexcept {
    math::Vec3 a0 = math::Vec3::Zero();
    math::Vec3 a1 = math::Vec3::Zero();
    math::Vec3 b0 = math::Vec3::Zero();
    math::Vec3 b1 = math::Vec3::Zero();
    CapsuleSegment(a, a0, a1);
    CapsuleSegment(b, b0, b1);

    math::Vec3 onA = math::Vec3::Zero();
    math::Vec3 onB = math::Vec3::Zero();
    ClosestPointsBetweenSegments(a0, a1, b0, b1, onA, onB);
    return SphereVsSphere(SphereAt(a, onA), SphereAt(b, onB));
}

/// Normal points from the capsule towards the box.
[[nodiscard]] ContactManifold CapsuleVsBox(const ShapePose& capsule,
                                          const ShapePose& box) noexcept {
    math::Vec3 from = math::Vec3::Zero();
    math::Vec3 to = math::Vec3::Zero();
    CapsuleSegment(capsule, from, to);

    // Alternating projection between two convex sets finds the *closest* pair of
    // points, which is what you want while the shapes are apart.  Once they
    // overlap the closest distance is zero and says nothing about depth, so the
    // axis ends are tested as well and the deepest result wins: for a capsule
    // sunk into a box the end is the part that is furthest inside, and using the
    // projected point instead under-reports the depth by up to a whole radius.
    math::Vec3 projected = (from + to) * 0.5f;
    math::Vec3 onBox =
        ClosestPointOnBox(projected, box.position, box.rotation, box.shape.halfExtents);
    for (int round = 0; round < 4; ++round) {
        projected = ClosestPointOnSegment(onBox, from, to);
        onBox = ClosestPointOnBox(projected, box.position, box.rotation, box.shape.halfExtents);
    }

    ContactManifold manifold;
    for (const math::Vec3& candidate : {from, to, projected}) {
        ContactManifold attempt = BoxVsSphere(box, SphereAt(capsule, candidate));
        // "Deeper than the best so far", with an empty manifold counting as
        // worse than a zero depth touch - otherwise an exactly resting capsule
        // is reported as not touching at all.
        if (attempt.IsValid() && (!manifold.IsValid() || attempt.Deepest().penetration >
                                                            manifold.Deepest().penetration)) {
            manifold = attempt;
        }
    }
    // BoxVsSphere reports box -> capsule; this pair reports capsule -> box.
    manifold.normal = -manifold.normal;
    return manifold;
}

/// A convex polygon in a box's local frame, used for face clipping.
///
/// Eight slots: a quad gains at most one vertex per clipping plane, and a box
/// face has four sides.
struct ClipPolygon {
    std::array<math::Vec3, 8> points{};
    u32 count = 0;
};

/// Keeps the part of the polygon on the `normal . p <= offset` side, inserting a
/// vertex wherever an edge crosses the plane.
void ClipAgainstPlane(ClipPolygon& polygon, math::Vec3 normal, f32 offset) noexcept {
    ClipPolygon clipped;
    for (u32 i = 0; i < polygon.count; ++i) {
        const math::Vec3 current = polygon.points[i];
        const math::Vec3 next = polygon.points[(i + 1) % polygon.count];
        const f32 currentDistance = math::Dot(normal, current) - offset;
        const f32 nextDistance = math::Dot(normal, next) - offset;
        if (currentDistance <= 0.0f && clipped.count < clipped.points.size()) {
            clipped.points[clipped.count++] = current;
        }
        if ((currentDistance < 0.0f) != (nextDistance < 0.0f) &&
            clipped.count < clipped.points.size()) {
            const f32 ratio = currentDistance / (currentDistance - nextDistance);
            clipped.points[clipped.count++] = current + (next - current) * ratio;
        }
    }
    polygon = clipped;
}

/// A box at a pose, with its world space axes cached.
struct OrientedBox {
    math::Vec3 position = math::Vec3::Zero();
    std::array<math::Vec3, 3> axes{math::Vec3::Right(), math::Vec3::Up(), math::Vec3::Forward()};
    math::Vec3 halfExtents = math::Vec3::Zero();
};

[[nodiscard]] OrientedBox ToOrientedBox(const ShapePose& pose) noexcept {
    OrientedBox box;
    box.position = pose.position;
    box.axes[0] = pose.rotation.Rotate(math::Vec3::Right());
    box.axes[1] = pose.rotation.Rotate(math::Vec3::Up());
    box.axes[2] = pose.rotation.Rotate(math::Vec3::Forward());
    box.halfExtents = pose.shape.halfExtents;
    return box;
}

/// Half the width of a box's shadow on a world space axis.
[[nodiscard]] f32 ProjectedRadius(const OrientedBox& box, math::Vec3 axis) noexcept {
    return box.halfExtents.x * std::abs(math::Dot(box.axes[0], axis)) +
           box.halfExtents.y * std::abs(math::Dot(box.axes[1], axis)) +
           box.halfExtents.z * std::abs(math::Dot(box.axes[2], axis));
}

/// Separating axis test over the 15 candidate axes of two oriented boxes.
[[nodiscard]] ContactManifold BoxVsBox(const ShapePose& aPose, const ShapePose& bPose) noexcept {
    ContactManifold manifold;
    const OrientedBox a = ToOrientedBox(aPose);
    const OrientedBox b = ToOrientedBox(bPose);
    const math::Vec3 delta = b.position - a.position;

    /// Which box the winning separating axis belongs to, because the contact
    /// point has to lie on that box's face.
    enum class AxisSource : u8 { FaceA, FaceB, Edge };

    struct Candidate {
        math::Vec3 direction;
        /// Edge crosses are penalised when choosing between axes of near equal
        /// overlap.  For two axis aligned boxes six of the nine cross products
        /// are not zero at all - they are the third axis again - so an edge axis
        /// reports exactly the overlap of the face axis it duplicates, and
        /// without a penalty the two are indistinguishable.  Taking the face
        /// matters: only a face has something to clip against.  The factor is
        /// above one because the comparison takes the *smallest* score.
        f32 preference = 1.0f;
        AxisSource source = AxisSource::Edge;
    };

    std::array<Candidate, 15> candidates{};
    for (usize i = 0; i < 3; ++i) {
        candidates[i] = Candidate{a.axes[i], 1.0f, AxisSource::FaceA};
        candidates[3 + i] = Candidate{b.axes[i], 1.0f, AxisSource::FaceB};
    }
    usize edge = 6;
    for (usize i = 0; i < 3; ++i) {
        for (usize j = 0; j < 3; ++j) {
            candidates[edge++] =
                Candidate{math::Cross(a.axes[i], b.axes[j]), 1.05f, AxisSource::Edge};
        }
    }

    f32 bestScore = math::kInfinity;
    f32 bestPenetration = 0.0f;
    math::Vec3 bestAxis = math::Vec3::Zero();
    AxisSource bestSource = AxisSource::Edge;
    usize bestAxisIndex = 0;

    for (usize index = 0; index < candidates.size(); ++index) {
        const Candidate& candidate = candidates[index];
        const f32 lengthSquared = math::LengthSquared(candidate.direction);
        if (lengthSquared < kTolerance) {
            // Parallel edges: the cross product carries no information, and the
            // face axes already cover this configuration.
            continue;
        }
        const math::Vec3 axis = candidate.direction / std::sqrt(lengthSquared);
        const f32 distance = std::abs(math::Dot(delta, axis));
        const f32 radii = ProjectedRadius(a, axis) + ProjectedRadius(b, axis);
        const f32 overlap = radii - distance;
        if (overlap <= -kContactMargin) {
            // A separating axis exists, so the boxes are apart.  The test uses the
            // unbiased overlap: the preference below is for choosing between
            // contacting axes, never for deciding whether they contact at all.
            return manifold;
        }
        const f32 score = overlap * candidate.preference;
        if (score < bestScore) {
            bestScore = score;
            bestPenetration = overlap;
            bestAxis = axis;
            bestSource = candidate.source;
            bestAxisIndex = index % 3;
        }
    }

    // The normal points from a towards b, whatever the axis search produced.
    if (math::Dot(bestAxis, delta) < 0.0f) {
        bestAxis = -bestAxis;
    }
    manifold.normal = bestAxis;

    const math::Vec3 onA =
        ClosestPointOnBox(b.position, a.position, aPose.rotation, a.halfExtents);
    const math::Vec3 onB =
        ClosestPointOnBox(a.position, b.position, bPose.rotation, b.halfExtents);

    // Edge against edge has no face to clip, and the overlap is a sliver in that
    // configuration anyway, so one point between the surfaces is the answer.
    if (bestSource == AxisSource::Edge) {
        manifold.points[0] = ManifoldPoint{(onA + onB) * 0.5f,
                                           bestPenetration > 0.0f ? bestPenetration : 0.0f};
        manifold.pointCount = 1;
        return manifold;
    }

    // --- Face clipping ------------------------------------------------------
    // The box that owns the contact axis lends its face as the reference; the
    // other box's most opposed face is the incident one.  Clipping the incident
    // face against the four sides of the reference face leaves the overlap
    // region, and every corner of it that is still inside the reference box is a
    // contact point.  This is what lets a box rest on four corners instead of
    // balancing on one, which is the difference between a crate that sits still
    // and one that spins itself off the table.
    const usize axisIndex = bestAxisIndex;
    const bool referenceIsA = bestSource == AxisSource::FaceA;
    const OrientedBox& reference = referenceIsA ? a : b;
    const OrientedBox& incident = referenceIsA ? b : a;
    const math::Quaternion& referenceRotation = referenceIsA ? aPose.rotation : bPose.rotation;
    const math::Quaternion& incidentRotation = referenceIsA ? bPose.rotation : aPose.rotation;

    // Outward normal of the reference face, in the reference box's own frame.
    //
    // "Outward" means away from the reference box and towards the incident one:
    // that is `bestAxis` when the reference is `a`, and its *opposite* when the
    // reference is `b`, because the face of b that touches a points back at a.
    // Getting this the wrong way round measures the depth across the whole body
    // instead of across the overlap, so a 6 mm touch is reported as a 2 m
    // penetration and the position solver throws both boxes off the screen.
    const math::Vec3 outward = referenceIsA ? bestAxis : -bestAxis;
    const f32 facing = math::Dot(reference.axes[axisIndex], outward) >= 0.0f ? 1.0f : -1.0f;
    math::Vec3 referenceNormal = math::Vec3::Zero();
    referenceNormal[axisIndex] = facing;
    const f32 faceOffset = reference.halfExtents[axisIndex];

    // The incident box's centre and axes, expressed in the reference frame, which
    // is where the side planes are simply `|coordinate| <= half extent`.
    const math::Quaternion toReference = referenceRotation.Inverse();
    const math::Vec3 incidentCentre = toReference.Rotate(incident.position - reference.position);
    const std::array<math::Vec3, 3> incidentAxes{
        toReference.Rotate(incidentRotation.Rotate(math::Vec3::Right())),
        toReference.Rotate(incidentRotation.Rotate(math::Vec3::Up())),
        toReference.Rotate(incidentRotation.Rotate(math::Vec3::Forward()))};

    usize incidentAxis = 0;
    f32 bestAlignment = -1.0f;
    for (usize axis = 0; axis < 3; ++axis) {
        const f32 alignment = std::abs(math::Dot(incidentAxes[axis], referenceNormal));
        if (alignment > bestAlignment) {
            bestAlignment = alignment;
            incidentAxis = axis;
        }
    }
    const f32 incidentSign =
        math::Dot(incidentAxes[incidentAxis], referenceNormal) > 0.0f ? -1.0f : 1.0f;
    const usize u = (incidentAxis + 1) % 3;
    const usize v = (incidentAxis + 2) % 3;

    ClipPolygon polygon;
    const f32 faceDepth = incidentSign * incident.halfExtents[incidentAxis];
    // Around the face, not across it: a nested pair of loops would visit the
    // corners in a bowtie order, and clipping a self-intersecting polygon
    // produces points that are not on the incident face at all.
    const std::array<std::pair<f32, f32>, 4> winding{{{-1.0f, -1.0f}, {-1.0f, 1.0f},
                                                     {1.0f, 1.0f}, {1.0f, -1.0f}}};
    for (const auto& [su, sv] : winding) {
        polygon.points[polygon.count++] =
            incidentCentre + incidentAxes[incidentAxis] * faceDepth +
            incidentAxes[u] * (su * incident.halfExtents[u]) +
            incidentAxes[v] * (sv * incident.halfExtents[v]);
    }

    for (usize axis = 0; axis < 3; ++axis) {
        if (axis == axisIndex) {
            continue;
        }
        math::Vec3 side = math::Vec3::Zero();
        side[axis] = 1.0f;
        ClipAgainstPlane(polygon, side, reference.halfExtents[axis]);
        ClipAgainstPlane(polygon, -side, reference.halfExtents[axis]);
    }

    for (u32 i = 0; i < polygon.count && manifold.pointCount < ContactManifold::kMaxPoints; ++i) {
        // referenceNormal is the true outward normal, so the face plane sits at
        // +faceOffset along it and anything below that is penetrating.
        const f32 depth = faceOffset - math::Dot(referenceNormal, polygon.points[i]);
        if (depth <= -kContactMargin) {
            continue; // Clearly above the reference face.
        }
        manifold.points[manifold.pointCount++] = ManifoldPoint{
            reference.position + referenceRotation.Rotate(polygon.points[i]),
            depth > 0.0f ? depth : 0.0f};
    }

    if (manifold.pointCount == 0) {
        // Clipping can come back empty when the boxes only just touch.  The SAT
        // depth is still the right answer, so fall back to a single point.
        manifold.points[0] = ManifoldPoint{(onA + onB) * 0.5f, bestPenetration};
        manifold.pointCount = 1;
    }
    return manifold;
}

} // namespace

math::Vec3 ClosestPointOnSegment(math::Vec3 point, math::Vec3 from, math::Vec3 to) noexcept {
    const math::Vec3 delta = to - from;
    const f32 lengthSquared = math::LengthSquared(delta);
    if (lengthSquared < kTolerance) {
        return from;
    }
    const f32 t = ClampScalar(math::Dot(point - from, delta) / lengthSquared, 0.0f, 1.0f);
    return from + delta * t;
}

void ClosestPointsBetweenSegments(math::Vec3 p1, math::Vec3 q1, math::Vec3 p2, math::Vec3 q2,
                                 math::Vec3& outOnFirst, math::Vec3& outOnSecond) noexcept {
    const math::Vec3 d1 = q1 - p1;
    const math::Vec3 d2 = q2 - p2;
    const math::Vec3 r = p1 - p2;
    const f32 a = math::LengthSquared(d1);
    const f32 e = math::LengthSquared(d2);
    const f32 f = math::Dot(d2, r);

    f32 s = 0.0f;
    f32 t = 0.0f;

    if (a < kTolerance && e < kTolerance) {
        // Both segments are points.
        outOnFirst = p1;
        outOnSecond = p2;
        return;
    }

    if (a < kTolerance) {
        // The first segment is a point: project it onto the second.
        t = ClampScalar(f / e, 0.0f, 1.0f);
    } else {
        const f32 c = math::Dot(d1, r);
        if (e < kTolerance) {
            // The second segment is a point.
            s = ClampScalar(-c / a, 0.0f, 1.0f);
        } else {
            const f32 b = math::Dot(d1, d2);
            const f32 denominator = a * e - b * b;
            // Parallel segments have no unique closest pair; either end will do,
            // so take the start of the first rather than dividing by zero.
            s = denominator > kTolerance ? ClampScalar((b * f - c * e) / denominator, 0.0f, 1.0f)
                                        : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = ClampScalar(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = ClampScalar((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    outOnFirst = p1 + d1 * s;
    outOnSecond = p2 + d2 * t;
}

math::Vec3 ClosestPointOnBox(math::Vec3 point, math::Vec3 boxPosition,
                            const math::Quaternion& boxRotation,
                            math::Vec3 halfExtents) noexcept {
    const math::Vec3 local = boxRotation.Inverse().Rotate(point - boxPosition);
    return boxPosition + boxRotation.Rotate(math::Clamp(local, -halfExtents, halfExtents));
}

ContactManifold Intersect(const ShapePose& a, const ShapePose& b) noexcept {
    // Every helper below has a fixed normal convention; the negations keep the
    // public one (a towards b) true for all nine shape orderings.
    switch (a.shape.type) {
        case ShapeType::Sphere:
            switch (b.shape.type) {
                case ShapeType::Sphere: return SphereVsSphere(a, b);
                case ShapeType::Box: {
                    ContactManifold manifold = BoxVsSphere(b, a);
                    manifold.normal = -manifold.normal;
                    return manifold;
                }
                case ShapeType::Capsule: {
                    ContactManifold manifold = CapsuleVsSphere(b, a);
                    manifold.normal = -manifold.normal;
                    return manifold;
                }
            }
            break;
        case ShapeType::Box:
            switch (b.shape.type) {
                case ShapeType::Sphere: return BoxVsSphere(a, b);
                case ShapeType::Box: return BoxVsBox(a, b);
                case ShapeType::Capsule: {
                    ContactManifold manifold = CapsuleVsBox(b, a);
                    manifold.normal = -manifold.normal;
                    return manifold;
                }
            }
            break;
        case ShapeType::Capsule:
            switch (b.shape.type) {
                case ShapeType::Sphere: return CapsuleVsSphere(a, b);
                case ShapeType::Box: return CapsuleVsBox(a, b);
                case ShapeType::Capsule: return CapsuleVsCapsule(a, b);
            }
            break;
    }
    return ContactManifold{};
}

} // namespace l3d::physics
