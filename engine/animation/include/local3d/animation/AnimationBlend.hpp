#pragma once
/// @file AnimationBlend.hpp
/// @brief Combining poses: weighted blends, additive layers and bone masks.
///
/// Everything here works on *local* poses.  Blending in model space would be
/// wrong in a way that only shows up on characters with deep hierarchies: the
/// result for a child bone would depend on which parent pose happened to be
/// blended first.
///
/// Two conventions worth stating up front:
///   * Rotations blend with slerp, positions and scales with lerp.  Slerp takes
///     the short way round the hypersphere, so a key authored as q and one
///     authored as -q (the same rotation) blend identically.
///   * Additive poses store a *difference* from a reference pose, not an absolute
///     one: position differences, a relative rotation and a scale ratio.
///     ApplyAdditive(ref, MakeAdditiveDelta(ref, x), 1.0f) reproduces x exactly,
///     which is the round trip the test suite pins down.

#include "local3d/animation/AnimationTypes.hpp"
#include "local3d/animation/Skeleton.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/math/Transform.hpp"

#include <span>
#include <vector>

namespace l3d::anim {

/// Linear/spherical blend of two transforms.  `t` is clamped to [0, 1]; blending
/// is not extrapolation and pretending otherwise produces NaN rotations.
[[nodiscard]] math::Transform BlendTransform(const math::Transform& a, const math::Transform& b,
                                             f32 t) noexcept;

/// out = a * (1 - t) blended with b, per bone.
[[nodiscard]] OperationResult BlendPoses(const Pose& a, const Pose& b, f32 t, Pose& out);

/// Blends `out` towards `b` in place - the cheap path when the caller does not
/// need `a` any more.
[[nodiscard]] OperationResult BlendPosesInPlace(Pose& out, const Pose& b, f32 t);

/// Weighted average of several poses.  Weights are normalised internally, so
/// callers can pass raw blend tree weights.  InvalidArgument when the weights sum
/// to zero or the pose counts disagree.
///
/// Poses past the first are folded in one at a time, which is exact for two
/// poses and a very good approximation beyond that - the same trade every real
/// time blend tree makes, because there is no exact closed form average of more
/// than two rotations.
[[nodiscard]] OperationResult BlendPosesWeighted(std::span<const Pose> poses,
                                                 std::span<const f32> weights, Pose& out);

/// Per bone on/off switch for layered blending: an upper body wave that leaves
/// the legs running.
class BoneMask {
public:
    explicit BoneMask(usize boneCount = 0, bool enabled = true);

    [[nodiscard]] usize BoneCount() const noexcept { return enabled_.size(); }
    void Resize(usize boneCount, bool enabled = true);
    void SetAll(bool enabled);

    void SetEnabled(u32 bone, bool enabled);
    [[nodiscard]] bool IsEnabled(u32 bone) const noexcept;
    [[nodiscard]] usize EnabledCount() const noexcept;

    /// Enables or disables a bone and everything below it.  Without this, masking
    /// "Spine2" would still animate the arms hanging off it.
    [[nodiscard]] OperationResult SetSubtreeEnabled(const Skeleton& skeleton, u32 root,
                                                    bool enabled);

private:
    std::vector<u8> enabled_;
};

/// BlendPoses restricted to the masked bones; unmasked bones keep `a`.
[[nodiscard]] OperationResult BlendPosesMasked(const Pose& a, const Pose& b, f32 t,
                                               const BoneMask& mask, Pose& out);

/// Copies whole bones from `source` into `target` where the mask is enabled.
[[nodiscard]] OperationResult CopyMaskedBones(const Pose& source, const BoneMask& mask,
                                              Pose& target);

// --- Additive layers --------------------------------------------------------

/// Builds the delta that turns `reference` into `current`.
[[nodiscard]] OperationResult MakeAdditiveDelta(const Pose& reference, const Pose& current,
                                                Pose& outDelta);

/// Applies `delta` on top of `base`, scaled by `weight` in [0, 1].
[[nodiscard]] OperationResult ApplyAdditive(const Pose& base, const Pose& delta, f32 weight,
                                            Pose& out);

} // namespace l3d::anim
