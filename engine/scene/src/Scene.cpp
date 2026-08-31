#include "local3d/scene/Scene.hpp"

#include "local3d/core/Assert.hpp"

#include <algorithm>
#include <utility>

namespace l3d::scene {
namespace {

/// World matrix of a node that does not exist.  A single shared immutable value,
/// so the accessors can return a reference without inventing a temporary.
constexpr math::Mat4 kIdentityMatrix = math::Mat4::Identity();

/// A candidate draw item plus the entity that produced it, so the output can be
/// sorted before the two are split into parallel arrays.
struct Candidate {
    ecs::Entity entity = ecs::kNullEntity;
    render::DrawItem item;
};

} // namespace

Scene::Scene(std::string name) : name_(std::move(name)) {}

// --- Hierarchy -------------------------------------------------------------

Result<ecs::Entity> Scene::CreateNode(std::string name, ecs::Entity parent) {
    if (parent.IsValid() && !IsNode(parent)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Parent is not a node of this scene"});
    }

    const ecs::Entity node = world_.CreateEntity();
    // Name first: Emplace can grow a pool, and a reference into one pool must
    // not be held while another pool is being written.
    world_.Emplace<NameComponent>(node, NameComponent{std::move(name)});
    TransformComponent& transform = world_.Emplace<TransformComponent>(node);
    transform.parent = parent;

    if (parent.IsValid()) {
        if (TransformComponent* parentTransform = world_.Get<TransformComponent>(parent)) {
            parentTransform->children.push_back(node);
            // Start from the parent's world matrix: a node created between two
            // updates would otherwise sit at the origin for one frame, which is
            // exactly the "object flashes at the centre" editor bug.
            transform.world = parentTransform->world;
        }
    }

    MarkDirty(node);
    return node;
}

OperationResult Scene::SetParent(ecs::Entity node, ecs::Entity newParent) {
    if (!IsNode(node)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Entity is not a scene node"});
    }
    if (newParent.IsValid()) {
        if (newParent == node) {
            return Unexpected(Status{StatusCode::InvalidArgument, "A node cannot parent itself"});
        }
        if (!IsNode(newParent)) {
            return Unexpected(
                Status{StatusCode::InvalidArgument, "New parent is not a node of this scene"});
        }
        if (IsDescendant(newParent, node)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Reparenting would create a cycle"});
        }
    }

    DetachFromParent(node);
    TransformComponent& transform = *world_.Get<TransformComponent>(node);
    transform.parent = newParent;
    if (newParent.IsValid()) {
        world_.Get<TransformComponent>(newParent)->children.push_back(node);
    }
    MarkDirty(node);
    return {};
}

void Scene::DestroyNode(ecs::Entity node) {
    if (!IsNode(node)) {
        return;
    }
    DetachFromParent(node);
    DestroySubtree(node);
}

bool Scene::IsNode(ecs::Entity node) const noexcept {
    return world_.IsAlive(node) && world_.Get<TransformComponent>(node) != nullptr;
}

std::span<const ecs::Entity> Scene::Children(ecs::Entity node) const noexcept {
    const TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return {};
    }
    return std::span<const ecs::Entity>(transform->children.data(), transform->children.size());
}

ecs::Entity Scene::Parent(ecs::Entity node) const noexcept {
    const TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return ecs::kNullEntity;
    }
    // A parent handle that no longer refers to a node (someone destroyed it
    // through the world directly) is reported as no parent, which is also how
    // Roots() treats it.  The graph stays traversable instead of hanging.
    if (!transform->parent.IsValid() || world_.Get<TransformComponent>(transform->parent) == nullptr) {
        return ecs::kNullEntity;
    }
    return transform->parent;
}

std::vector<ecs::Entity> Scene::Roots() const {
    std::vector<ecs::Entity> roots;
    // AllEntities() is index order, so this is deterministic.
    for (const ecs::Entity entity : world_.AllEntities()) {
        if (world_.Get<TransformComponent>(entity) == nullptr) {
            continue;
        }
        if (!Parent(entity).IsValid()) {
            roots.push_back(entity);
        }
    }
    return roots;
}

std::vector<ecs::Entity> Scene::Nodes() const {
    std::vector<ecs::Entity> nodes;
    nodes.reserve(NodeCount());
    for (const ecs::Entity entity : world_.AllEntities()) {
        if (world_.Get<TransformComponent>(entity) != nullptr) {
            nodes.push_back(entity);
        }
    }
    return nodes;
}

bool Scene::IsDescendant(ecs::Entity node, ecs::Entity ancestor) const noexcept {
    ecs::Entity current = Parent(node);
    while (current.IsValid()) {
        if (current == ancestor) {
            return true;
        }
        current = Parent(current);
    }
    return false;
}

u32 Scene::Depth(ecs::Entity node) const noexcept {
    u32 depth = 0;
    ecs::Entity current = Parent(node);
    while (current.IsValid()) {
        ++depth;
        current = Parent(current);
    }
    return depth;
}

void Scene::DetachFromParent(ecs::Entity node) {
    TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr || !transform->parent.IsValid()) {
        return;
    }
    if (TransformComponent* parent = world_.Get<TransformComponent>(transform->parent)) {
        std::vector<ecs::Entity>& children = parent->children;
        children.erase(std::remove(children.begin(), children.end(), node), children.end());
    }
    transform->parent = ecs::kNullEntity;
}

void Scene::DestroySubtree(ecs::Entity node) {
    TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return;
    }
    // The child list dies with the component, so it has to be copied before the
    // first child is destroyed.
    const std::vector<ecs::Entity> children = transform->children;
    transform->children.clear();
    for (const ecs::Entity child : children) {
        DestroySubtree(child);
    }
    if (activeCamera_ == node) {
        activeCamera_ = ecs::kNullEntity;
    }
    world_.DestroyEntity(node);
}

void Scene::Clear() {
    world_.Clear();
    activeCamera_ = ecs::kNullEntity;
    dirtyNodes_ = 0;
}

// --- Transforms ------------------------------------------------------------

OperationResult Scene::SetLocalTransform(ecs::Entity node, const math::Transform& local) {
    TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Entity is not a scene node"});
    }
    transform->local = local;
    MarkDirty(node);
    return {};
}

const math::Transform* Scene::LocalTransform(ecs::Entity node) const noexcept {
    const TransformComponent* transform = world_.Get<TransformComponent>(node);
    return transform != nullptr ? &transform->local : nullptr;
}

const math::Mat4& Scene::WorldMatrix(ecs::Entity node) const noexcept {
    const TransformComponent* transform = world_.Get<TransformComponent>(node);
    return transform != nullptr ? transform->world : kIdentityMatrix;
}

math::Vec3 Scene::WorldPosition(ecs::Entity node) const noexcept {
    return WorldMatrix(node).TransformPoint(math::Vec3::Zero());
}

void Scene::MarkDirty(ecs::Entity node) noexcept {
    MarkSubtreeDirty(node);
    MarkAncestorsDirty(node);
}

void Scene::MarkSubtreeDirty(ecs::Entity node) noexcept {
    TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return;
    }
    if (!transform->dirty) {
        transform->dirty = true;
        ++dirtyNodes_;
    }
    for (const ecs::Entity child : transform->children) {
        MarkSubtreeDirty(child);
    }
}

void Scene::MarkAncestorsDirty(ecs::Entity node) noexcept {
    TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return;
    }
    ecs::Entity parent = Parent(node);
    while (parent.IsValid()) {
        TransformComponent* parentTransform = world_.Get<TransformComponent>(parent);
        if (parentTransform == nullptr) {
            return;
        }
        if (parentTransform->dirty) {
            // Already dirty, and dirty implies its own ancestors are dirty too,
            // so the walk can stop here.
            return;
        }
        parentTransform->dirty = true;
        ++dirtyNodes_;
        parent = Parent(parent);
    }
}

usize Scene::UpdateTransforms() {
    usize updated = 0;
    for (const ecs::Entity root : Roots()) {
        updated += UpdateSubtree(root, kIdentityMatrix);
    }
    return updated;
}

usize Scene::UpdateSubtree(ecs::Entity node, const math::Mat4& parentWorld) {
    TransformComponent* transform = world_.Get<TransformComponent>(node);
    if (transform == nullptr) {
        return 0;
    }
    if (!transform->dirty) {
        // Sound because MarkDirty never dirties a node without dirtying its
        // descendants: a clean node means a clean subtree.
        return 0;
    }

    transform->world = parentWorld * transform->local.ToMatrix();
    transform->dirty = false;
    L3D_ASSERT_MSG(dirtyNodes_ > 0, "Dirty counter underflow in Scene::UpdateSubtree");
    if (dirtyNodes_ > 0) {
        --dirtyNodes_;
    }

    usize updated = 1;
    // No structural change happens below, so the cached matrix stays put and can
    // be handed down by reference.
    const math::Mat4& world = transform->world;
    for (const ecs::Entity child : transform->children) {
        updated += UpdateSubtree(child, world);
    }
    return updated;
}

// --- Frame inputs ----------------------------------------------------------

void Scene::CollectDrawItems(const IMeshResolver& meshes, std::vector<render::DrawItem>& items,
                             std::vector<ecs::Entity>& sources, u32& unresolved) const {
    items.clear();
    sources.clear();
    unresolved = 0;

    std::vector<Candidate> candidates;
    // Driven by the mesh renderers (the sparse set), so a scene with ten
    // thousand nodes and three renderables costs three iterations.
    world_.Each<MeshRendererComponent>(
        [&](ecs::Entity entity, const MeshRendererComponent& renderer) {
            if (!renderer.visible || !renderer.HasMesh()) {
                return;
            }
            const TransformComponent* transform = world_.Get<TransformComponent>(entity);
            if (transform == nullptr) {
                return; // A renderer without a node has no place in the world.
            }

            Candidate candidate;
            candidate.entity = entity;
            bool anyResolved = false;
            bool haveBounds = false;
            for (u32 slot = 0; slot < MeshRendererComponent::kMaxLods; ++slot) {
                if (renderer.lods[slot].IsNull()) {
                    break;
                }
                const ResolvedMesh resolved = meshes.Resolve(renderer.lods[slot]);
                if (!resolved.IsValid()) {
                    ++unresolved;
                    continue;
                }
                anyResolved = true;
                candidate.item.lods[slot] = resolved.handle;
                // Culling uses one box for the whole item, so the lods have to
                // agree on it: the union never culls something a coarser lod
                // would have drawn.
                candidate.item.localBounds =
                    haveBounds ? candidate.item.localBounds.Merged(resolved.bounds) : resolved.bounds;
                haveBounds = true;
            }
            if (!anyResolved) {
                return;
            }

            candidate.item.lodSwitchCoverage = renderer.lodSwitchCoverage;
            candidate.item.world = transform->world;
            candidate.item.tint = renderer.tint;
            candidate.item.castsShadow = renderer.castsShadow;
            candidates.push_back(std::move(candidate));
        });

    // The renderer batches in input order, so the order has to be a property of
    // the scene and not of the sparse set's insertion history.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.entity < b.entity; });

    items.reserve(candidates.size());
    sources.reserve(candidates.size());
    for (Candidate& candidate : candidates) {
        items.push_back(std::move(candidate.item));
        sources.push_back(candidate.entity);
    }
}

std::optional<render::DirectionalLight> Scene::FindSunLight() const {
    // CollectLights() is already in node order, so the first directional light
    // in that order is the one that wins, deterministically.
    std::optional<SceneLight> best;
    for (const SceneLight& light : CollectLights()) {
        if (light.light.type == LightComponent::Type::Directional) {
            best = light;
            break;
        }
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    render::DirectionalLight sun;
    sun.direction = best->direction;
    sun.color = best->light.color;
    sun.intensity = best->light.intensity;
    sun.castsShadow = best->light.castsShadow;
    return sun;
}

std::vector<Scene::SceneLight> Scene::CollectLights() const {
    std::vector<SceneLight> lights;
    world_.Each<LightComponent>([&](ecs::Entity entity, const LightComponent& light) {
        const TransformComponent* transform = world_.Get<TransformComponent>(entity);
        if (transform == nullptr) {
            return;
        }
        SceneLight entry;
        entry.entity = entity;
        entry.light = light;
        entry.position = transform->world.TransformPoint(math::Vec3::Zero());
        entry.direction = light.DirectionFromWorld(transform->world);
        lights.push_back(entry);
    });
    std::sort(lights.begin(), lights.end(),
              [](const SceneLight& a, const SceneLight& b) { return a.entity < b.entity; });
    return lights;
}

OperationResult Scene::SetActiveCamera(ecs::Entity node) {
    if (!IsNode(node) || world_.Get<CameraComponent>(node) == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Entity has no camera component"});
    }
    if (activeCamera_.IsValid() && activeCamera_ != node) {
        if (CameraComponent* previous = world_.Get<CameraComponent>(activeCamera_)) {
            previous->active = false;
        }
    }
    world_.Get<CameraComponent>(node)->active = true;
    activeCamera_ = node;
    return {};
}

Result<render::FrameView> Scene::BuildFrameView(ecs::Entity cameraNode, u32 width,
                                               u32 height) const {
    const CameraComponent* camera = world_.Get<CameraComponent>(cameraNode);
    const TransformComponent* transform = world_.Get<TransformComponent>(cameraNode);
    if (camera == nullptr || transform == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Entity is not a camera node"});
    }
    if (!camera->IsValid()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Camera projection is invalid"});
    }
    if (width == 0 || height == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Viewport size must be non zero"});
    }

    render::FrameView view;
    view.position = transform->world.TransformPoint(math::Vec3::Zero());
    view.target = view.position + transform->world.TransformDirection(math::Vec3::Forward());
    view.up = math::Normalize(transform->world.TransformDirection(math::Vec3::Up()));
    view.fovYDegrees = camera->fovYDegrees;
    view.aspect = camera->AspectFor(width, height);
    view.nearPlane = camera->nearPlane;
    view.farPlane = camera->farPlane;
    view.viewportWidth = width;
    view.viewportHeight = height;
    view.Update();
    return view;
}

} // namespace l3d::scene
