#pragma once
/// @file InverseKinematics.hpp
/// @brief Analytic IK applied to a local pose: two bone chains and look-at.
///
/// These are solvers, not systems: each takes a skeleton, a pose and a job, and
/// rotates bones so a chain reaches a target.  They run after the animation
/// sampling and before the skinning matrices are built, which is the only order
/// that makes sense - IK that runs before the pose is final gets overwritten.
///
/// Both operate in model space and write the result back as local rotations,
/// because a pose is local.  Positions and scales are never touched: moving a
/// bone's position to satisfy a target is what makes ragdoll-looking arms.
///
/// Why analytic rather than CCD/FABRIK: a two bone chain has a closed form
/// solution, it is deterministic, it converges in one pass, and it is what every
/// shipped foot placement and hand attachment system uses.  Iterative solvers are
/// for chains nobody can name.

#include "local3d/animation/AnimationTypes.hpp"
#include "local3d/animation/Skeleton.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/math/Quaternion.hpp"
#include "local3d/math/Vector.hpp"

namespace l3d::anim {

/// Smallest rotation carrying `from` onto `to`.  Handles the antiparallel case
/// explicitly, where the cross product degenerates and the naive formula returns
/// a zero quaternion - a silent "no rotation" instead of a 180 degree turn.
[[nodiscard]] math::Quaternion RotationBetween(math::Vec3 from, math::Vec3 to) noexcept;

/// Rotates `rotation` about its own axis so the total angle is at most
/// `maxRadians`, preserving direction.  Used to keep a head from snapping round.
[[nodiscard]] math::Quaternion ClampRotationAngle(const math::Quaternion& rotation,
                                                  f32 maxRadians) noexcept;

/// A root -> mid -> tip chain and where the tip should end up.  The three bones
/// must be direct parent/child in exactly that order.
struct TwoBoneIkJob {
    u32 root = kInvalidBone;
    u32 mid = kInvalidBone;
    u32 tip = kInvalidBone;
    /// Model space target for the tip bone's position.
    math::Vec3 target = math::Vec3::Zero();
    /// Model space point the middle joint bends towards - the knee or elbow hint.
    /// Without it the bend plane is undefined: the same target is reachable with
    /// the elbow up or down.
    math::Vec3 poleHint = math::Vec3::Up();
    /// 0 leaves the pose alone, 1 applies the full solution.
    f32 weight = 1.0f;
};

/// Solves the chain and rotates `root` and `mid` in place.
///
/// Unreachable targets are handled by pulling the tip as close as the chain
/// length allows rather than by failing: a hand that cannot quite reach a ledge
/// should still stretch, and a solver that returns an error there would leave the
/// character in the previous frame's pose.  Zero length bones are a genuine data
/// error and do return one.
[[nodiscard]] OperationResult ApplyTwoBoneIk(const Skeleton& skeleton, Pose& pose,
                                             const TwoBoneIkJob& job);

struct LookAtJob {
    u32 bone = kInvalidBone;
    /// Model space point the bone should aim at.
    math::Vec3 target = math::Vec3::Zero();
    /// Which local axis of the bone points at the target.
    math::Vec3 forwardAxis = math::Vec3::Forward();
    /// Local up used to keep the bone from rolling.
    math::Vec3 upAxis = math::Vec3::Up();
    /// Hard limit on how far the bone may turn away from its animated pose, in
    /// radians.  Keeps a head track from turning into an owl.
    f32 maxAngleRadians = math::kPi * 0.35f;
    f32 weight = 1.0f;
};

[[nodiscard]] OperationResult ApplyLookAt(const Skeleton& skeleton, Pose& pose,
                                          const LookAtJob& job);

} // namespace l3d::anim
