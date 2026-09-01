#pragma once
/// @file Skeleton.hpp
/// @brief Bone hierarchy, bind pose and the pose -> matrix passes that feed GPU
///        skinning.
///
/// One invariant makes every pass in this class a single forward loop: a bone's
/// parent must already exist when the bone is added, so `parent < index` always
/// holds.  AddBone enforces it, which is why there is no topological sort
/// anywhere and no recursion in the evaluation code.
///
/// A Skeleton is data, not a resource handle: it is small, copyable, and shared
/// by every character that uses the same rig.  Poses are evaluated against it;
/// the skeleton never changes once a mesh is bound to it.

#include "local3d/animation/AnimationTypes.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/math/Matrix.hpp"
#include "local3d/math/Transform.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::anim {

/// Everything the engine knows about one bone.  `children` is stored explicitly
/// so that IK and ragdoll code can walk down without scanning the array.
struct BoneInfo {
    std::string name;
    u32 parent = kInvalidBone;
    u32 depth = 0;
    std::vector<u32> children;
    /// Bind pose relative to the parent bone.
    math::Transform bindLocal;
};

class Skeleton {
public:
    Skeleton() = default;
    explicit Skeleton(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] const std::string& Name() const noexcept { return name_; }
    void SetName(std::string name) { name_ = std::move(name); }

    /// Appends a bone.  `parent` must be kInvalidBone or an existing bone, which
    /// is what keeps parents ahead of children in the array.
    ///   InvalidArgument - empty name, unknown parent, self parenting.
    ///   AlreadyExists   - a bone with this name already exists (names are the
    ///                     lookup key and must stay unique for retargeting).
    [[nodiscard]] Result<u32> AddBone(std::string name, u32 parent = kInvalidBone);

    [[nodiscard]] Result<u32> FindBone(std::string_view name) const;
    [[nodiscard]] bool HasBone(std::string_view name) const noexcept;

    [[nodiscard]] u32 BoneCount() const noexcept { return static_cast<u32>(bones_.size()); }
    [[nodiscard]] bool IsEmpty() const noexcept { return bones_.empty(); }

    [[nodiscard]] const BoneInfo& Bone(u32 bone) const noexcept {
        L3D_ASSERT_MSG(bone < bones_.size(), "Skeleton::Bone index out of range");
        return bones_[bone];
    }

    [[nodiscard]] const std::string& BoneName(u32 bone) const noexcept { return Bone(bone).name; }
    [[nodiscard]] u32 Parent(u32 bone) const noexcept { return Bone(bone).parent; }

    [[nodiscard]] std::span<const u32> Children(u32 bone) const noexcept {
        return Bone(bone).children;
    }

    /// Distance to the root: 0 for a root bone.
    [[nodiscard]] u32 Depth(u32 bone) const noexcept { return Bone(bone).depth; }

    [[nodiscard]] std::span<const BoneInfo> Bones() const noexcept { return bones_; }

    /// Sets a bone's bind pose relative to its parent and invalidates the cached
    /// bind matrices.
    [[nodiscard]] OperationResult SetBindTransform(u32 bone, const math::Transform& local);

    [[nodiscard]] const math::Transform& BindTransform(u32 bone) const noexcept {
        return Bone(bone).bindLocal;
    }

    /// The bind pose as a local pose - the pose every blend starts from, so that
    /// bones no clip animates stay where the artist put them.
    [[nodiscard]] Pose BindPose() const;

    /// Model space bind matrices, computed on first use and cached.
    [[nodiscard]] const std::vector<math::Mat4>& InverseBindMatrices() const;

    // --- Evaluation -------------------------------------------------------

    /// Local pose -> model space transforms.  `out` must hold BoneCount()
    /// entries; InvalidArgument says so rather than writing out of bounds.
    [[nodiscard]] OperationResult ComputeModelSpace(const Pose& pose,
                                                    std::span<math::Transform> out) const;

    /// Local pose -> model space bone matrices.
    [[nodiscard]] OperationResult ComputeModelSpaceMatrices(const Pose& pose,
                                                            std::span<math::Mat4> out) const;

    /// Local pose -> the matrices a skinning shader multiplies a vertex by:
    /// modelSpace[i] * inverseBind[i].  In the bind pose these are all identity,
    /// which is the property the test suite checks.
    [[nodiscard]] OperationResult ComputeSkinningMatrices(const Pose& pose,
                                                          std::span<math::Mat4> out) const;

    /// Model space bone positions only - what two bone IK needs as input.
    [[nodiscard]] OperationResult ComputeModelSpacePositions(const Pose& pose,
                                                             std::span<math::Vec3> out) const;

private:
    void InvalidateBindCache() noexcept;
    void RebuildBindCache() const;

    std::string name_;
    std::vector<BoneInfo> bones_;
    mutable std::vector<math::Mat4> inverseBind_;
    mutable bool bindCacheValid_ = false;
};

} // namespace l3d::anim
