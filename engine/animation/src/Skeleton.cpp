#include "local3d/animation/Skeleton.hpp"

#include <utility>

namespace l3d::anim {

namespace {

Status InvalidArgument(std::string_view message) {
    return Status{StatusCode::InvalidArgument, message};
}

/// Validates a transform before it enters animation data.
[[nodiscard]] OperationResult CheckTransform(const math::Transform& local) {
    if (!IsFiniteValue(local.position)) {
        return Unexpected(InvalidArgument("Transform position is not finite"));
    }
    if (!IsFiniteValue(local.rotation)) {
        return Unexpected(InvalidArgument("Transform rotation is not finite"));
    }
    if (!IsFiniteValue(local.scale)) {
        return Unexpected(InvalidArgument("Transform scale is not finite"));
    }
    if (local.scale.x == 0.0f || local.scale.y == 0.0f || local.scale.z == 0.0f) {
        // A zero scale axis makes the inverse bind matrix singular, and every
        // vertex attached to the bone collapses onto a plane.
        return Unexpected(InvalidArgument("Bind transform has a zero scale axis"));
    }
    return {};
}

} // namespace

Result<u32> Skeleton::AddBone(std::string name, u32 parent) {
    if (name.empty()) {
        return Unexpected(InvalidArgument("Bone name is empty"));
    }
    if (parent != kInvalidBone) {
        if (parent >= bones_.size()) {
            return Unexpected(InvalidArgument("Parent bone does not exist"));
        }
    }
    for (const BoneInfo& existing : bones_) {
        if (existing.name == name) {
            return Unexpected(Status{StatusCode::AlreadyExists, "A bone with this name exists"});
        }
    }

    const u32 index = static_cast<u32>(bones_.size());
    BoneInfo info;
    info.name = std::move(name);
    info.parent = parent;
    info.depth = parent == kInvalidBone ? 0u : bones_[parent].depth + 1u;
    bones_.push_back(std::move(info));
    if (parent != kInvalidBone) {
        bones_[parent].children.push_back(index);
    }
    InvalidateBindCache();
    return index;
}

Result<u32> Skeleton::FindBone(std::string_view name) const {
    for (u32 i = 0; i < bones_.size(); ++i) {
        if (bones_[i].name == name) {
            return i;
        }
    }
    return Unexpected(Status{StatusCode::NotFound, "No bone with this name"});
}

bool Skeleton::HasBone(std::string_view name) const noexcept {
    for (const BoneInfo& bone : bones_) {
        if (bone.name == name) {
            return true;
        }
    }
    return false;
}

OperationResult Skeleton::SetBindTransform(u32 bone, const math::Transform& local) {
    if (bone >= bones_.size()) {
        return Unexpected(InvalidArgument("Bone does not exist"));
    }
    L3D_RETURN_IF_ERROR(CheckTransform(local));
    bones_[bone].bindLocal = local;
    InvalidateBindCache();
    return {};
}

Pose Skeleton::BindPose() const {
    std::vector<math::Transform> local(bones_.size());
    for (usize i = 0; i < bones_.size(); ++i) {
        local[i] = bones_[i].bindLocal;
    }
    return Pose{std::move(local)};
}

const std::vector<math::Mat4>& Skeleton::InverseBindMatrices() const {
    if (!bindCacheValid_) {
        RebuildBindCache();
    }
    return inverseBind_;
}

OperationResult Skeleton::ComputeModelSpace(const Pose& pose, std::span<math::Transform> out) const {
    if (pose.BoneCount() != bones_.size()) {
        return Unexpected(InvalidArgument("Pose does not match the skeleton bone count"));
    }
    if (out.size() != bones_.size()) {
        return Unexpected(InvalidArgument("Output does not match the skeleton bone count"));
    }
    for (u32 i = 0; i < bones_.size(); ++i) {
        const u32 parent = bones_[i].parent;
        // Safe as a single forward pass: AddBone only accepts parents that were
        // added earlier, so parent < i holds for every bone.
        out[i] = parent == kInvalidBone ? pose.At(i)
                                        : math::Transform::Combine(out[parent], pose.At(i));
    }
    return {};
}

OperationResult Skeleton::ComputeModelSpaceMatrices(const Pose& pose,
                                                    std::span<math::Mat4> out) const {
    if (out.size() != bones_.size()) {
        return Unexpected(InvalidArgument("Output does not match the skeleton bone count"));
    }
    std::vector<math::Transform> modelSpace(bones_.size());
    L3D_RETURN_IF_ERROR(ComputeModelSpace(pose, modelSpace));
    for (usize i = 0; i < modelSpace.size(); ++i) {
        out[i] = modelSpace[i].ToMatrix();
    }
    return {};
}

OperationResult Skeleton::ComputeSkinningMatrices(const Pose& pose,
                                                   std::span<math::Mat4> out) const {
    if (out.size() != bones_.size()) {
        return Unexpected(InvalidArgument("Output does not match the skeleton bone count"));
    }
    std::vector<math::Mat4> modelSpace(bones_.size());
    L3D_RETURN_IF_ERROR(ComputeModelSpaceMatrices(pose, modelSpace));
    const std::vector<math::Mat4>& inverseBind = InverseBindMatrices();
    for (usize i = 0; i < modelSpace.size(); ++i) {
        out[i] = modelSpace[i] * inverseBind[i];
    }
    return {};
}

OperationResult Skeleton::ComputeModelSpacePositions(const Pose& pose,
                                                     std::span<math::Vec3> out) const {
    if (out.size() != bones_.size()) {
        return Unexpected(InvalidArgument("Output does not match the skeleton bone count"));
    }
    std::vector<math::Transform> modelSpace(bones_.size());
    L3D_RETURN_IF_ERROR(ComputeModelSpace(pose, modelSpace));
    for (usize i = 0; i < modelSpace.size(); ++i) {
        out[i] = modelSpace[i].position;
    }
    return {};
}

void Skeleton::InvalidateBindCache() noexcept { bindCacheValid_ = false; }

void Skeleton::RebuildBindCache() const {
    std::vector<math::Transform> modelSpace(bones_.size());
    for (u32 i = 0; i < bones_.size(); ++i) {
        const u32 parent = bones_[i].parent;
        modelSpace[i] = parent == kInvalidBone
                            ? bones_[i].bindLocal
                            : math::Transform::Combine(modelSpace[parent], bones_[i].bindLocal);
    }
    inverseBind_.assign(bones_.size(), math::Mat4::Identity());
    for (usize i = 0; i < modelSpace.size(); ++i) {
        inverseBind_[i] = modelSpace[i].ToMatrix().Inverse();
    }
    bindCacheValid_ = true;
}

} // namespace l3d::anim
