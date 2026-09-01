#pragma once
/// @file AnimationPlayer.hpp
/// @brief What a character actually owns: a skeleton, a stack of blended layers,
///        IK and the skinning matrices for the GPU.
///
/// The frame order is fixed and matters:
///
///     1. every layer's state machine advances and reports its events
///     2. each layer samples a pose over the bind pose
///     3. layers are folded into the result, bottom up (masked or additive)
///     4. IK runs on the finished pose
///     5. skinning matrices are rebuilt
///
/// IK last, because IK that runs before the pose is final gets overwritten by the
/// next layer.  Skinning matrices last, because they are the thing the renderer
/// uploads and they have to see the IK solution.
///
/// Ownership: the player borrows the skeleton, the state machines and the clips
/// behind them.  It owns only its own scratch buffers - which is the point, since
/// allocating poses per character per frame is the cost that shows up first in a
/// profiler.  A layer's machine must outlive the player.
///
/// Threading: Update() is safe to call from a worker as long as nothing else
/// touches this player or its machines.  The outputs are plain arrays, so the
/// upload to the GPU can happen on any thread afterwards.

#include "local3d/animation/AnimationBlend.hpp"
#include "local3d/animation/AnimationStateMachine.hpp"
#include "local3d/animation/InverseKinematics.hpp"
#include "local3d/animation/Skeleton.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/math/Matrix.hpp"

#include <span>
#include <vector>

namespace l3d::anim {

class AnimationPlayer {
public:
    explicit AnimationPlayer(const Skeleton& skeleton);

    /// What a layer does.  `machine` is borrowed, must outlive the player, and is
    /// advanced by Update() - the player owns the *clock* of every machine it is
    /// given, even though it does not own the object.
    struct LayerDesc {
        AnimationStateMachine* machine = nullptr;
        /// 0 leaves the pose below untouched, 1 replaces it entirely (or, for an
        /// additive layer, applies the full delta).
        f32 weight = 1.0f;
        /// Additive layers store a delta against a reference pose instead of
        /// replacing the pose below: a breathing layer over a run cycle.
        bool additive = false;
    };

    /// Layers are applied in the order they were added; layer 0 is the base.
    /// InvalidArgument for a null machine, a non finite weight, or more layers
    /// than kMaxLayers.
    [[nodiscard]] Result<u32> AddLayer(LayerDesc desc);

    [[nodiscard]] usize LayerCount() const noexcept { return layers_.size(); }

    [[nodiscard]] OperationResult SetLayerWeight(u32 layer, f32 weight);
    [[nodiscard]] f32 LayerWeight(u32 layer) const;

    /// Restricts a layer to part of the skeleton.  Without a mask a layer covers
    /// every bone.
    [[nodiscard]] OperationResult SetLayerMask(u32 layer, BoneMask mask);
    [[nodiscard]] OperationResult ClearLayerMask(u32 layer);

    /// Reference pose an additive layer measures its delta against.  Defaults to
    /// the skeleton bind pose, which is what an additive clip exported "relative
    /// to bind" expects.
    [[nodiscard]] OperationResult SetLayerAdditiveReference(u32 layer, Pose reference);

    // --- Inverse kinematics ----------------------------------------------

    /// Jobs run in the order added, after all layers are blended.
    [[nodiscard]] OperationResult AddTwoBoneIk(TwoBoneIkJob job);
    [[nodiscard]] OperationResult AddLookAt(LookAtJob job);
    void ClearIkJobs();

    // --- Frame ------------------------------------------------------------

    /// Advances, blends, solves IK and rebuilds the skinning matrices.  Failures
    /// inside a layer or an IK job are logged and skipped: one broken job should
    /// not stop a frame.
    void Update(f32 deltaTime);

    /// Local pose after blending and IK.
    [[nodiscard]] const Pose& CurrentPose() const noexcept { return result_; }

    /// Model space bind-pose-relative matrices, ready to upload as a skinning
    /// buffer.  Identity for every bone when the pose is the bind pose.
    [[nodiscard]] std::span<const math::Mat4> SkinningMatrices() const noexcept {
        return skinning_;
    }

    /// Events raised by the Update that just ran, across every layer.
    [[nodiscard]] std::span<const FiredAnimationEvent> FiredEvents() const noexcept {
        return fired_;
    }

private:
    struct Layer {
        AnimationStateMachine* machine = nullptr;
        f32 weight = 1.0f;
        bool additive = false;
        bool hasMask = false;
        BoneMask mask;
        bool hasAdditiveReference = false;
        Pose additiveReference;
    };

    [[nodiscard]] OperationResult CheckLayer(u32 layer) const;
    void ApplyLayer(const Layer& layer);

    static constexpr usize kMaxLayers = 8;

    const Skeleton& skeleton_;
    Pose bindPose_;
    Pose result_;
    /// Scratch buffers, sized once and reused: layer output, additive delta and a
    /// blend target.
    Pose scratchLayer_;
    Pose scratchDelta_;
    Pose scratchBlend_;
    Pose scratchSample_;

    std::vector<Layer> layers_;
    std::vector<TwoBoneIkJob> twoBoneJobs_;
    std::vector<LookAtJob> lookAtJobs_;
    std::vector<math::Mat4> skinning_;
    std::vector<FiredAnimationEvent> fired_;
};

} // namespace l3d::anim
