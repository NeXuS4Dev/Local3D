#include "local3d/animation/InverseKinematics.hpp"

#include "local3d/animation/AnimationTypes.hpp"
#include "local3d/math/Constants.hpp"

#include <cmath>

namespace l3d::anim {

namespace {

Status InvalidArgument(std::string_view message) {
    return Status{StatusCode::InvalidArgument, message};
}

/// Any unit vector perpendicular to `axis`, chosen deterministically.
[[nodiscard]] math::Vec3 PerpendicularTo(math::Vec3 axis) noexcept {
    const math::Vec3 candidate =
        std::abs(math::Dot(axis, math::Vec3::Right())) < 0.9f ? math::Vec3::Right()
                                                              : math::Vec3::Up();
    return math::Normalize(math::Cross(axis, candidate));
}

/// World rotation of a bone's parent, identity for a root.
[[nodiscard]] math::Quaternion ParentWorldRotation(const Skeleton& skeleton, u32 bone,
                                                   std::span<const math::Transform> modelSpace)
    noexcept {
    const u32 parent = skeleton.Parent(bone);
    return parent == kInvalidBone ? math::Quaternion::Identity() : modelSpace[parent].rotation;
}

} // namespace

math::Quaternion RotationBetween(math::Vec3 from, math::Vec3 to) noexcept {
    const math::Vec3 a = math::Normalize(from);
    const math::Vec3 b = math::Normalize(to);
    const f32 dot = math::Clamp(math::Dot(a, b), -1.0f, 1.0f);
    if (dot > 1.0f - math::kEpsilon) {
        return math::Quaternion::Identity();
    }
    if (dot < -1.0f + math::kEpsilon) {
        // Antiparallel: every perpendicular axis is equally valid, so pick one
        // deterministically instead of returning a zero quaternion.
        return math::Quaternion::FromAxisAngle(PerpendicularTo(a), math::kPi);
    }
    const math::Vec3 axis = math::Cross(a, b);
    return math::Quaternion{axis.x, axis.y, axis.z, 1.0f + dot}.Normalized();
}

math::Quaternion ClampRotationAngle(const math::Quaternion& rotation, f32 maxRadians) noexcept {
    if (!(maxRadians >= 0.0f)) {
        return math::Quaternion::Identity();
    }
    // Fold into the w >= 0 hemisphere so the angle is unambiguous in [0, pi].
    math::Quaternion q = rotation;
    if (q.w < 0.0f) {
        q = math::Quaternion{-q.x, -q.y, -q.z, -q.w};
    }
    q = q.Normalized();
    const f32 angle = 2.0f * std::acos(math::Clamp(q.w, -1.0f, 1.0f));
    if (angle <= maxRadians) {
        return q;
    }
    const math::Vec3 axis{q.x, q.y, q.z};
    if (math::LengthSquared(axis) < math::kEpsilon) {
        return math::Quaternion::Identity();
    }
    return math::Quaternion::FromAxisAngle(math::Normalize(axis), maxRadians);
}

OperationResult ApplyTwoBoneIk(const Skeleton& skeleton, Pose& pose, const TwoBoneIkJob& job) {
    if (job.root >= skeleton.BoneCount() || job.mid >= skeleton.BoneCount() ||
        job.tip >= skeleton.BoneCount()) {
        return Unexpected(InvalidArgument("IK chain refers to an unknown bone"));
    }
    if (job.root == job.mid || job.mid == job.tip || job.root == job.tip) {
        return Unexpected(InvalidArgument("IK chain bones must be distinct"));
    }
    if (skeleton.Parent(job.mid) != job.root || skeleton.Parent(job.tip) != job.mid) {
        return Unexpected(InvalidArgument("IK chain is not root -> mid -> tip"));
    }
    if (!IsFiniteValue(job.target) || !IsFiniteValue(job.poleHint)) {
        return Unexpected(InvalidArgument("IK target or pole hint is not finite"));
    }

    std::vector<math::Transform> modelSpace(skeleton.BoneCount());
    L3D_RETURN_IF_ERROR(skeleton.ComputeModelSpace(pose, modelSpace));

    const math::Vec3 root = modelSpace[job.root].position;
    const math::Vec3 mid = modelSpace[job.mid].position;
    const math::Vec3 tip = modelSpace[job.tip].position;
    const f32 upperLength = math::Distance(root, mid);
    const f32 lowerLength = math::Distance(mid, tip);
    if (upperLength < math::kEpsilon || lowerLength < math::kEpsilon) {
        return Unexpected(InvalidArgument("IK chain has a zero length bone"));
    }

    // Pull the target in to somewhere the chain can actually reach.
    const math::Vec3 toTarget = job.target - root;
    const f32 distance = math::Length(toTarget);
    const f32 reach = math::Clamp(distance, std::abs(upperLength - lowerLength),
                                  upperLength + lowerLength);
    const math::Vec3 axis = distance > math::kEpsilon ? toTarget / distance
                                                      : PerpendicularTo(math::Vec3::Up());
    const math::Vec3 reachableTip = root + (axis * reach);

    // Bend plane: the component of the pole hint that is not along the root->tip
    // line.  If the hint lies on that line the plane is undefined, so fall back to
    // a perpendicular rather than producing a NaN basis.
    const math::Vec3 pole = job.poleHint - root;
    math::Vec3 bend = pole - (axis * math::Dot(pole, axis));
    if (math::LengthSquared(bend) < math::kEpsilon) {
        bend = PerpendicularTo(axis);
    }
    bend = math::Normalize(bend);

    // Law of cosines for the angle at the root joint.
    const f32 cosRoot =
        math::Clamp(((upperLength * upperLength) + (reach * reach) - (lowerLength * lowerLength)) /
                        (2.0f * upperLength * reach),
                    -1.0f, 1.0f);
    const f32 sinRoot = std::sqrt(math::Clamp(1.0f - (cosRoot * cosRoot), 0.0f, 1.0f));
    const math::Vec3 desiredUpper = math::Normalize((axis * cosRoot) + (bend * sinRoot));

    const math::Vec3 currentUpper = mid - root;
    const math::Quaternion deltaRoot = RotationBetween(currentUpper, desiredUpper);
    const math::Vec3 solvedMid = root + (desiredUpper * upperLength);
    const math::Vec3 desiredLower = reachableTip - solvedMid;
    const math::Vec3 currentLower = deltaRoot.Rotate(tip - mid);
    const math::Quaternion deltaMid = RotationBetween(currentLower, desiredLower);

    const f32 weight = math::Clamp01(job.weight);
    const math::Quaternion appliedRoot =
        math::Quaternion::Slerp(math::Quaternion::Identity(), deltaRoot, weight);
    const math::Quaternion appliedMid =
        math::Quaternion::Slerp(math::Quaternion::Identity(), deltaMid, weight);

    const math::Quaternion newWorldRoot =
        (appliedRoot * modelSpace[job.root].rotation).Normalized();
    // The mid bone is inside the root's subtree, so the root's rotation has
    // already been applied to it by the time the mid correction runs.  Leaving
    // appliedRoot out here is the classic two bone IK bug: the elbow ends up in
    // the right place and the hand lands somewhere else entirely.
    const math::Quaternion newWorldMid =
        (appliedMid * appliedRoot * modelSpace[job.mid].rotation).Normalized();

    pose.At(job.root).rotation =
        (ParentWorldRotation(skeleton, job.root, modelSpace).Inverse() * newWorldRoot).Normalized();
    pose.At(job.mid).rotation = (newWorldRoot.Inverse() * newWorldMid).Normalized();
    return {};
}

OperationResult ApplyLookAt(const Skeleton& skeleton, Pose& pose, const LookAtJob& job) {
    if (job.bone >= skeleton.BoneCount()) {
        return Unexpected(InvalidArgument("Look at bone does not exist"));
    }
    if (!IsFiniteValue(job.target) || !IsFiniteValue(job.forwardAxis) ||
        !IsFiniteValue(job.upAxis)) {
        return Unexpected(InvalidArgument("Look at job contains a non finite vector"));
    }
    if (math::LengthSquared(job.forwardAxis) < math::kEpsilon ||
        math::LengthSquared(job.upAxis) < math::kEpsilon) {
        return Unexpected(InvalidArgument("Look at axes must be non zero"));
    }

    std::vector<math::Transform> modelSpace(skeleton.BoneCount());
    L3D_RETURN_IF_ERROR(skeleton.ComputeModelSpace(pose, modelSpace));

    const math::Transform& world = modelSpace[job.bone];
    const math::Vec3 toTarget = job.target - world.position;
    if (math::LengthSquared(toTarget) < math::kEpsilon) {
        return Unexpected(InvalidArgument("Look at target coincides with the bone"));
    }

    // LookRotation aims local -Z, so pre-rotate the requested forward axis onto it.
    const math::Quaternion align = RotationBetween(job.forwardAxis, math::Vec3::Forward());
    const math::Quaternion desiredWorld = math::Quaternion::LookRotation(toTarget, job.upAxis) *
                                          align;
    math::Quaternion delta = (desiredWorld * world.rotation.Inverse()).Normalized();
    delta = ClampRotationAngle(delta, job.maxAngleRadians);

    const f32 weight = math::Clamp01(job.weight);
    delta = math::Quaternion::Slerp(math::Quaternion::Identity(), delta, weight);
    const math::Quaternion newWorld = (delta * world.rotation).Normalized();
    pose.At(job.bone).rotation =
        (ParentWorldRotation(skeleton, job.bone, modelSpace).Inverse() * newWorld).Normalized();
    return {};
}

} // namespace l3d::anim
