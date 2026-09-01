#include "local3d/animation/AnimationBlend.hpp"

#include "local3d/math/Constants.hpp"

#include <cmath>

namespace l3d::anim {

namespace {

Status InvalidArgument(std::string_view message) {
    return Status{StatusCode::InvalidArgument, message};
}

/// The one check every blend in this file needs.  Poses of different sizes would
/// either read past the end or silently blend unrelated bones together.
[[nodiscard]] OperationResult CheckBlendable(const Pose& a, const Pose& b, const Pose& out) {
    if (a.BoneCount() != b.BoneCount()) {
        return Unexpected(InvalidArgument("Poses have different bone counts"));
    }
    if (a.BoneCount() != out.BoneCount()) {
        return Unexpected(InvalidArgument("Output pose has a different bone count"));
    }
    return {};
}

} // namespace

math::Transform BlendTransform(const math::Transform& a, const math::Transform& b, f32 t) noexcept {
    const f32 weight = math::Clamp01(t);
    math::Transform result;
    result.position = math::Lerp(a.position, b.position, weight);
    result.rotation = math::Quaternion::Slerp(a.rotation, b.rotation, weight);
    result.scale = math::Lerp(a.scale, b.scale, weight);
    return result;
}

OperationResult BlendPoses(const Pose& a, const Pose& b, f32 t, Pose& out) {
    L3D_RETURN_IF_ERROR(CheckBlendable(a, b, out));
    for (u32 bone = 0; bone < out.BoneCount(); ++bone) {
        out.At(bone) = BlendTransform(a.At(bone), b.At(bone), t);
    }
    return {};
}

OperationResult BlendPosesInPlace(Pose& out, const Pose& b, f32 t) {
    return BlendPoses(out, b, t, out);
}

OperationResult BlendPosesWeighted(std::span<const Pose> poses, std::span<const f32> weights,
                                   Pose& out) {
    if (poses.empty()) {
        return Unexpected(InvalidArgument("No poses to blend"));
    }
    if (poses.size() != weights.size()) {
        return Unexpected(InvalidArgument("Pose count and weight count differ"));
    }
    f32 total = 0.0f;
    for (const f32 weight : weights) {
        if (!std::isfinite(weight) || weight < 0.0f) {
            return Unexpected(InvalidArgument("Blend weights must be finite and non negative"));
        }
        total += weight;
    }
    if (total <= math::kEpsilon) {
        return Unexpected(InvalidArgument("Blend weights sum to zero"));
    }
    const usize boneCount = poses.front().BoneCount();
    for (const Pose& pose : poses) {
        if (pose.BoneCount() != boneCount) {
            return Unexpected(InvalidArgument("Poses have different bone counts"));
        }
    }
    if (out.BoneCount() != boneCount) {
        return Unexpected(InvalidArgument("Output pose has a different bone count"));
    }

    out = poses.front();
    f32 accumulated = weights.front();
    for (usize i = 1; i < poses.size(); ++i) {
        // Fold pose i in with the share it owns of everything seen so far.  This
        // is the only order independent way to average rotations without storing
        // every intermediate.
        accumulated += weights[i];
        const f32 t = accumulated > math::kEpsilon ? weights[i] / accumulated : 0.0f;
        for (u32 bone = 0; bone < boneCount; ++bone) {
            out.At(bone) = BlendTransform(out.At(bone), poses[i].At(bone), t);
        }
    }
    return {};
}

BoneMask::BoneMask(usize boneCount, bool enabled) : enabled_(boneCount, enabled ? 1u : 0u) {}

void BoneMask::Resize(usize boneCount, bool enabled) {
    enabled_.resize(boneCount, enabled ? 1u : 0u);
}

void BoneMask::SetAll(bool enabled) {
    const u8 value = enabled ? 1u : 0u;
    for (u8& entry : enabled_) {
        entry = value;
    }
}

void BoneMask::SetEnabled(u32 bone, bool enabled) {
    if (bone < enabled_.size()) {
        enabled_[bone] = enabled ? 1u : 0u;
    }
}

bool BoneMask::IsEnabled(u32 bone) const noexcept {
    return bone < enabled_.size() && enabled_[bone] != 0u;
}

usize BoneMask::EnabledCount() const noexcept {
    usize count = 0;
    for (const u8 entry : enabled_) {
        if (entry != 0u) {
            ++count;
        }
    }
    return count;
}

OperationResult BoneMask::SetSubtreeEnabled(const Skeleton& skeleton, u32 root, bool enabled) {
    if (root >= skeleton.BoneCount()) {
        return Unexpected(InvalidArgument("Bone does not exist"));
    }
    if (enabled_.size() != skeleton.BoneCount()) {
        return Unexpected(InvalidArgument("Mask does not match the skeleton bone count"));
    }
    // Breadth first over the children lists; no recursion, so a 200 bone rig on a
    // small stack is not a problem.
    std::vector<u32> pending{root};
    while (!pending.empty()) {
        const u32 bone = pending.back();
        pending.pop_back();
        SetEnabled(bone, enabled);
        for (const u32 child : skeleton.Children(bone)) {
            pending.push_back(child);
        }
    }
    return {};
}

OperationResult BlendPosesMasked(const Pose& a, const Pose& b, f32 t, const BoneMask& mask,
                                 Pose& out) {
    L3D_RETURN_IF_ERROR(CheckBlendable(a, b, out));
    if (mask.BoneCount() != a.BoneCount()) {
        return Unexpected(InvalidArgument("Mask does not match the pose bone count"));
    }
    for (u32 bone = 0; bone < out.BoneCount(); ++bone) {
        out.At(bone) = mask.IsEnabled(bone) ? BlendTransform(a.At(bone), b.At(bone), t)
                                            : a.At(bone);
    }
    return {};
}

OperationResult CopyMaskedBones(const Pose& source, const BoneMask& mask, Pose& target) {
    if (source.BoneCount() != target.BoneCount()) {
        return Unexpected(InvalidArgument("Poses have different bone counts"));
    }
    if (mask.BoneCount() != source.BoneCount()) {
        return Unexpected(InvalidArgument("Mask does not match the pose bone count"));
    }
    for (u32 bone = 0; bone < source.BoneCount(); ++bone) {
        if (mask.IsEnabled(bone)) {
            target.At(bone) = source.At(bone);
        }
    }
    return {};
}

OperationResult MakeAdditiveDelta(const Pose& reference, const Pose& current, Pose& outDelta) {
    L3D_RETURN_IF_ERROR(CheckBlendable(reference, current, outDelta));
    for (u32 bone = 0; bone < outDelta.BoneCount(); ++bone) {
        const math::Transform& from = reference.At(bone);
        const math::Transform& to = current.At(bone);
        math::Transform delta;
        delta.position = to.position - from.position;
        delta.rotation = (from.rotation.Inverse() * to.rotation).Normalized();
        // A ratio, not a difference: scale multiplies down a hierarchy.
        delta.scale = math::DivideOrZero(to.scale, from.scale);
        outDelta.At(bone) = delta;
    }
    return {};
}

OperationResult ApplyAdditive(const Pose& base, const Pose& delta, f32 weight, Pose& out) {
    L3D_RETURN_IF_ERROR(CheckBlendable(base, delta, out));
    if (!std::isfinite(weight)) {
        return Unexpected(InvalidArgument("Additive weight is not finite"));
    }
    const f32 w = math::Clamp01(weight);
    for (u32 bone = 0; bone < out.BoneCount(); ++bone) {
        const math::Transform& from = base.At(bone);
        const math::Transform& add = delta.At(bone);
        math::Transform result;
        result.position = from.position + (add.position * w);
        // Slerping from identity is what makes the layer fade in smoothly instead
        // of snapping on at any non-zero weight.
        const math::Quaternion partial =
            math::Quaternion::Slerp(math::Quaternion::Identity(), add.rotation, w);
        result.rotation = (from.rotation * partial).Normalized();
        result.scale = from.scale * math::Lerp(math::Vec3::One(), add.scale, w);
        out.At(bone) = result;
    }
    return {};
}

} // namespace l3d::anim
