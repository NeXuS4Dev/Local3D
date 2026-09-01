#include "local3d/animation/AnimationPlayer.hpp"

#include "local3d/core/Log.hpp"

#include <cmath>
#include <utility>

namespace l3d::anim {

namespace {

Status InvalidArgument(std::string_view message) {
    return Status{StatusCode::InvalidArgument, message};
}

void Warn(const Status& status) {
    L3D_LOG_WARN(LogCategory::Animation, "AnimationPlayer: {}", status.Message());
}

} // namespace

AnimationPlayer::AnimationPlayer(const Skeleton& skeleton)
    : skeleton_(skeleton),
      bindPose_(skeleton.BindPose()),
      result_(bindPose_),
      scratchLayer_(bindPose_),
      scratchDelta_(bindPose_),
      scratchBlend_(bindPose_),
      scratchSample_(bindPose_),
      skinning_(skeleton.BoneCount(), math::Mat4::Identity()) {}

Result<u32> AnimationPlayer::AddLayer(LayerDesc desc) {
    if (desc.machine == nullptr) {
        return Unexpected(InvalidArgument("Layer has no state machine"));
    }
    if (desc.machine->StateCount() == 0) {
        return Unexpected(InvalidArgument("Layer state machine has no states"));
    }
    if (!std::isfinite(desc.weight)) {
        return Unexpected(InvalidArgument("Layer weight is not finite"));
    }
    if (layers_.size() >= kMaxLayers) {
        return Unexpected(InvalidArgument("Too many animation layers"));
    }
    Layer layer;
    layer.machine = desc.machine;
    layer.weight = desc.weight;
    layer.additive = desc.additive;
    layer.mask = BoneMask{bindPose_.BoneCount(), true};
    const u32 id = static_cast<u32>(layers_.size());
    layers_.push_back(std::move(layer));
    return id;
}

OperationResult AnimationPlayer::CheckLayer(u32 layer) const {
    if (layer >= layers_.size()) {
        return Unexpected(InvalidArgument("Layer does not exist"));
    }
    return {};
}

OperationResult AnimationPlayer::SetLayerWeight(u32 layer, f32 weight) {
    L3D_RETURN_IF_ERROR(CheckLayer(layer));
    if (!std::isfinite(weight)) {
        return Unexpected(InvalidArgument("Layer weight is not finite"));
    }
    layers_[layer].weight = weight;
    return {};
}

f32 AnimationPlayer::LayerWeight(u32 layer) const {
    return layer < layers_.size() ? layers_[layer].weight : 0.0f;
}

OperationResult AnimationPlayer::SetLayerMask(u32 layer, BoneMask mask) {
    L3D_RETURN_IF_ERROR(CheckLayer(layer));
    if (mask.BoneCount() != bindPose_.BoneCount()) {
        return Unexpected(InvalidArgument("Mask does not match the skeleton bone count"));
    }
    layers_[layer].mask = std::move(mask);
    layers_[layer].hasMask = true;
    return {};
}

OperationResult AnimationPlayer::ClearLayerMask(u32 layer) {
    L3D_RETURN_IF_ERROR(CheckLayer(layer));
    layers_[layer].hasMask = false;
    return {};
}

OperationResult AnimationPlayer::SetLayerAdditiveReference(u32 layer, Pose reference) {
    L3D_RETURN_IF_ERROR(CheckLayer(layer));
    if (reference.BoneCount() != bindPose_.BoneCount()) {
        return Unexpected(InvalidArgument("Reference pose does not match the skeleton"));
    }
    layers_[layer].additiveReference = std::move(reference);
    layers_[layer].hasAdditiveReference = true;
    return {};
}

OperationResult AnimationPlayer::AddTwoBoneIk(TwoBoneIkJob job) {
    if (job.root >= skeleton_.BoneCount() || job.mid >= skeleton_.BoneCount() ||
        job.tip >= skeleton_.BoneCount()) {
        return Unexpected(InvalidArgument("IK chain refers to an unknown bone"));
    }
    if (!std::isfinite(job.weight)) {
        return Unexpected(InvalidArgument("IK weight is not finite"));
    }
    twoBoneJobs_.push_back(job);
    return {};
}

OperationResult AnimationPlayer::AddLookAt(LookAtJob job) {
    if (job.bone >= skeleton_.BoneCount()) {
        return Unexpected(InvalidArgument("Look at bone does not exist"));
    }
    if (!std::isfinite(job.weight) || !std::isfinite(job.maxAngleRadians)) {
        return Unexpected(InvalidArgument("Look at job contains a non finite value"));
    }
    lookAtJobs_.push_back(job);
    return {};
}

void AnimationPlayer::ClearIkJobs() {
    twoBoneJobs_.clear();
    lookAtJobs_.clear();
}

void AnimationPlayer::Update(f32 deltaTime) {
    // The skeleton is meant to be immutable once a player exists, but rebuilding
    // here costs nothing and turns a use-after-grow into a correct frame.
    if (bindPose_.BoneCount() != skeleton_.BoneCount()) {
        bindPose_ = skeleton_.BindPose();
        result_ = bindPose_;
        scratchLayer_ = bindPose_;
        scratchDelta_ = bindPose_;
        scratchBlend_ = bindPose_;
        scratchSample_ = bindPose_;
        skinning_.assign(skeleton_.BoneCount(), math::Mat4::Identity());
    }

    fired_.clear();
    result_ = bindPose_;

    for (const Layer& layer : layers_) {
        if (layer.machine == nullptr) {
            continue;
        }
        layer.machine->Update(deltaTime);
        for (const FiredAnimationEvent& event : layer.machine->FiredEvents()) {
            fired_.push_back(event);
        }
        ApplyLayer(layer);
    }

    for (const TwoBoneIkJob& job : twoBoneJobs_) {
        const OperationResult applied = ApplyTwoBoneIk(skeleton_, result_, job);
        if (applied.IsError()) {
            Warn(applied.Error());
        }
    }
    for (const LookAtJob& job : lookAtJobs_) {
        const OperationResult applied = ApplyLookAt(skeleton_, result_, job);
        if (applied.IsError()) {
            Warn(applied.Error());
        }
    }

    skinning_.assign(skeleton_.BoneCount(), math::Mat4::Identity());
    const OperationResult skinned = skeleton_.ComputeSkinningMatrices(result_, skinning_);
    if (skinned.IsError()) {
        Warn(skinned.Error());
    }
}

void AnimationPlayer::ApplyLayer(const Layer& layer) {
    // Weight zero means "this layer contributes nothing", so skip the sampling
    // too - a disabled layer should cost one branch, not a pose.
    if (layer.weight <= 0.0f) {
        return;
    }
    const OperationResult sampled =
        layer.machine->SamplePose(bindPose_, scratchSample_, scratchLayer_);
    if (sampled.IsError()) {
        Warn(sampled.Error());
        return;
    }
    if (layer.additive) {
        const Pose& reference = layer.hasAdditiveReference ? layer.additiveReference : bindPose_;
        const OperationResult delta = MakeAdditiveDelta(reference, scratchLayer_, scratchDelta_);
        if (delta.IsError()) {
            Warn(delta.Error());
            return;
        }
        // result_ aliases the base argument, which is safe: ApplyAdditive reads a
        // bone and writes the same bone, never a different one.
        const OperationResult applied =
            ApplyAdditive(result_, scratchDelta_, layer.weight, result_);
        if (applied.IsError()) {
            Warn(applied.Error());
            return;
        }
        return;
    }
    const OperationResult blended =
        layer.hasMask
            ? BlendPosesMasked(result_, scratchLayer_, layer.weight, layer.mask, scratchBlend_)
            : BlendPoses(result_, scratchLayer_, layer.weight, scratchBlend_);
    if (blended.IsError()) {
        Warn(blended.Error());
        return;
    }
    result_ = scratchBlend_;
}

} // namespace l3d::anim
