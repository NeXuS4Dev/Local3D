#pragma once
/// @file Scene.hpp
/// @brief The scene graph: hierarchy, world transforms and frame inputs.
///
/// The Scene owns an ecs::World and adds the two things a plain ECS cannot
/// express well: a parent/child hierarchy with cached world matrices, and the
/// translation from "entities with components" to "what the renderer draws this
/// frame".
///
/// Ownership and lifetime, stated once because it is where scene code usually
/// goes wrong:
///   * The Scene owns the world, so it owns every entity and component in it.
///     Nothing outside holds a pointer into a component across a structural
///     change (CreateNode / DestroyNode / SetParent / Clear) - those can move
///     component storage.
///   * Destroying a node destroys its subtree.  That is the scene graph rule
///     every editor user expects; there is no orphaning.
///   * Entity handles carry a generation, so a handle to a destroyed node fails
///     IsNode() instead of silently aliasing a recycled slot.
///
/// Threading: single threaded, like the rest of the structural ECS.  The frame
/// inputs this class produces are plain values and can be handed to a worker.

#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/ecs/Entity.hpp"
#include "local3d/ecs/World.hpp"
#include "local3d/math/Geometry.hpp"
#include "local3d/renderer/FrameView.hpp"
#include "local3d/renderer/RenderTypes.hpp"
#include "local3d/scene/SceneComponents.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace l3d::scene {

/// What a mesh asset id resolves to at run time.  The bounds travel with the
/// handle because culling needs them and they come from the same cooked file.
struct ResolvedMesh {
    render::MeshHandle handle = render::kInvalidMesh;
    math::Aabb bounds;

    [[nodiscard]] bool IsValid() const noexcept { return handle != render::kInvalidMesh; }
};

/// Turns asset ids into GPU handles.  Implemented by SceneResources; the Scene
/// only ever sees this interface, which keeps the scene graph free of any
/// device dependency and lets a headless test resolve to fake handles.
class IMeshResolver {
public:
    virtual ~IMeshResolver() = default;

    /// Returns an invalid handle for an unknown or not yet loaded asset; the
    /// caller decides whether that is a warning or an error.
    [[nodiscard]] virtual ResolvedMesh Resolve(assets::AssetId id) const = 0;
};

class Scene {
public:
    explicit Scene(std::string name = "Scene");

    [[nodiscard]] const std::string& Name() const noexcept { return name_; }
    void SetName(std::string name) { name_ = std::move(name); }

    [[nodiscard]] ecs::World& World() noexcept { return world_; }
    [[nodiscard]] const ecs::World& World() const noexcept { return world_; }

    // --- Hierarchy --------------------------------------------------------

    /// Creates a node, parented to `parent` or to the scene root.
    /// `InvalidArgument` when the parent is not a node of this scene.
    [[nodiscard]] Result<ecs::Entity> CreateNode(std::string name = {},
                                                ecs::Entity parent = ecs::kNullEntity);

    /// Reparents a node.  Pass kNullEntity to make it a root.
    /// `InvalidArgument` on a missing node, on self parenting and on anything
    /// that would create a cycle.
    [[nodiscard]] OperationResult SetParent(ecs::Entity node, ecs::Entity newParent);

    /// Destroys a node and everything below it.  No-op for a handle that is not
    /// a live node of this scene.
    void DestroyNode(ecs::Entity node);

    /// True for a live entity that has a TransformComponent, i.e. a node.
    [[nodiscard]] bool IsNode(ecs::Entity node) const noexcept;

    /// Children in creation order.  Empty span for a non node.
    [[nodiscard]] std::span<const ecs::Entity> Children(ecs::Entity node) const noexcept;

    /// Parent handle, or kNullEntity for a root or a non node.
    [[nodiscard]] ecs::Entity Parent(ecs::Entity node) const noexcept;

    /// Top level nodes, in entity index order (stable across frames).
    [[nodiscard]] std::vector<ecs::Entity> Roots() const;

    /// Every node, in entity index order.  What the outliner and the serializer
    /// walk, so the order is part of the contract.
    [[nodiscard]] std::vector<ecs::Entity> Nodes() const;

    /// True when `ancestor` is on the path from `node` to a root.  A node is not
    /// its own descendant.
    [[nodiscard]] bool IsDescendant(ecs::Entity node, ecs::Entity ancestor) const noexcept;

    /// Distance to the root; 0 for a root, 0 for a non node.
    [[nodiscard]] u32 Depth(ecs::Entity node) const noexcept;

    /// Number of nodes.  Read from the component pool rather than counted, so it
    /// cannot drift from what is actually stored.
    [[nodiscard]] usize NodeCount() const noexcept {
        return world_.ComponentCount<TransformComponent>();
    }

    /// Adds a component to a node.  `InvalidArgument` when the entity is not a
    /// node, which is the mistake this exists to catch.
    template <typename T, typename... Args>
    [[nodiscard]] Result<T*> AddComponent(ecs::Entity node, Args&&... args) {
        if (!IsNode(node)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Entity is not a scene node"});
        }
        return &world_.Emplace<T>(node, std::forward<Args>(args)...);
    }

    // --- Transforms -------------------------------------------------------

    /// Sets the local transform and invalidates the node's subtree.
    [[nodiscard]] OperationResult SetLocalTransform(ecs::Entity node, const math::Transform& local);

    [[nodiscard]] const math::Transform* LocalTransform(ecs::Entity node) const noexcept;

    /// World matrix as of the last update.  Identity for a non node; call
    /// UpdateTransforms() before reading this if anything moved.
    [[nodiscard]] const math::Mat4& WorldMatrix(ecs::Entity node) const noexcept;

    /// World space position, read straight out of the cached matrix.
    [[nodiscard]] math::Vec3 WorldPosition(ecs::Entity node) const noexcept;

    /// Marks a node, its ancestors and its descendants as needing a recompute.
    /// Ancestors are included so the top down update can prune clean subtrees
    /// without missing a dirty node below a clean one.
    void MarkDirty(ecs::Entity node) noexcept;

    /// Recomputes every dirty world matrix.  Returns how many nodes were
    /// updated, which is what a profiler wants to see: zero means the scene was
    /// static this frame and the traversal cost was one branch per root.
    usize UpdateTransforms();

    /// True when at least one world matrix is stale.
    [[nodiscard]] bool HasPendingUpdates() const noexcept { return dirtyNodes_ > 0; }
    [[nodiscard]] usize DirtyNodeCount() const noexcept { return dirtyNodes_; }

    // --- Frame inputs -----------------------------------------------------

    /// Collects the visible, drawable nodes as renderer draw items.
    ///
    /// `items` and `sources` are filled in parallel (`sources[i]` is the entity
    /// that produced `items[i]`) and are sorted by entity index, so the input
    /// order the renderer batches from is stable frame to frame.  Both vectors
    /// are cleared first.
    ///
    /// A node whose mesh has not been resolved yet is skipped and counted in
    /// `unresolved`, because a missing asset is a warning the editor shows, not
    /// a reason to drop the rest of the frame.
    void CollectDrawItems(const IMeshResolver& meshes, std::vector<render::DrawItem>& items,
                          std::vector<ecs::Entity>& sources, u32& unresolved) const;

    /// The directional light that drives the shadow cascades: the first
    /// active directional light in node order.  Nullopt when the scene has none,
    /// in which case the caller decides whether that is an error.
    [[nodiscard]] std::optional<render::DirectionalLight> FindSunLight() const;

    /// Every light in the scene, in node order, with its world position and
    /// direction filled in.
    struct SceneLight {
        ecs::Entity entity = ecs::kNullEntity;
        LightComponent light;
        math::Vec3 position{};
        math::Vec3 direction{0.0f, -1.0f, 0.0f};
    };
    [[nodiscard]] std::vector<SceneLight> CollectLights() const;

    /// The camera that renders when none is named.  kNullEntity when there is
    /// none or it was destroyed.
    [[nodiscard]] ecs::Entity ActiveCamera() const noexcept { return activeCamera_; }

    /// Marks `node` as the active camera and clears the flag on any other.
    /// `InvalidArgument` when the node has no CameraComponent.
    [[nodiscard]] OperationResult SetActiveCamera(ecs::Entity node);

    /// Builds the renderer's view from a camera node.  `InvalidArgument` when
    /// the node is not a camera; the camera's own IsValid() rules out the
    /// near/far/fov combinations that would produce NaN matrices.
    [[nodiscard]] Result<render::FrameView> BuildFrameView(ecs::Entity cameraNode, u32 width,
                                                          u32 height) const;

    /// Destroys every node.  The scene keeps its name.
    void Clear();

private:
    void MarkSubtreeDirty(ecs::Entity node) noexcept;
    void MarkAncestorsDirty(ecs::Entity node) noexcept;
    usize UpdateSubtree(ecs::Entity node, const math::Mat4& parentWorld);
    /// Removes `node` from its parent's child list, if it has one.
    void DetachFromParent(ecs::Entity node);
    void DestroySubtree(ecs::Entity node);

    std::string name_;
    ecs::World world_;
    ecs::Entity activeCamera_ = ecs::kNullEntity;
    usize dirtyNodes_ = 0;
};

} // namespace l3d::scene
