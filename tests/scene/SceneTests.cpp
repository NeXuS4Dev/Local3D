// Scene graph tests.
//
// The hierarchy, the world matrix cache and the frame inputs are verified
// numerically, and one case runs the whole chain end to end: a cooked mesh file
// on a memory file system, through the runtime loader and the resource cache,
// into draw items the renderer actually records.
#include "doctest.h"

#include "local3d/assets/AssetData.hpp"
#include "local3d/assets/AssetManager.hpp"
#include "local3d/assets/Cooker.hpp"
#include "local3d/assets/FileSystem.hpp"
#include "local3d/math/Constants.hpp"
#include "local3d/renderer/Renderer.hpp"
#include "local3d/scene/Scene.hpp"
#include "local3d/scene/SceneResources.hpp"
#include "local3d/scene/SceneSerializer.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace l3d;
using namespace l3d::math;
using namespace l3d::scene;

namespace {

/// A resolver that hands out canned handles, so the graph logic can be tested
/// without a device.
class FakeResolver final : public IMeshResolver {
public:
    void Add(assets::AssetId id, render::MeshHandle handle, Aabb bounds) {
        entries_.emplace(id, ResolvedMesh{handle, bounds});
    }

    [[nodiscard]] ResolvedMesh Resolve(assets::AssetId id) const override {
        const auto found = entries_.find(id);
        return found != entries_.end() ? found->second : ResolvedMesh{};
    }

private:
    std::unordered_map<assets::AssetId, ResolvedMesh> entries_;
};

[[nodiscard]] Aabb Box(f32 halfExtent) {
    Aabb bounds;
    bounds.min = Vec3{-halfExtent, -halfExtent, -halfExtent};
    bounds.max = Vec3{halfExtent, halfExtent, halfExtent};
    return bounds;
}

[[nodiscard]] Vec3 TranslationOf(const Mat4& matrix) {
    return matrix.TransformPoint(Vec3::Zero());
}

[[nodiscard]] bool IsFinite(const Mat4& matrix) {
    for (usize i = 0; i < 16; ++i) {
        if (!std::isfinite(matrix.Data()[i])) {
            return false;
        }
    }
    return true;
}

/// Creates a node and unwraps it, failing the test case if the scene refused.
ecs::Entity Node(Scene& scene, std::string name = {},
                               ecs::Entity parent = ecs::kNullEntity) {
    auto created = scene.CreateNode(std::move(name), parent);
    REQUIRE_MESSAGE(created.HasValue(), "CreateNode failed: ", created.Error().Message());
    return *created;
}

/// Adds a component and unwraps it.  AddComponent returns a Result of a pointer,
/// so the value is two dereferences deep.
template <typename T>
T& Add(Scene& scene, ecs::Entity node) {
    auto added = scene.AddComponent<T>(node);
    REQUIRE_MESSAGE(added.HasValue(), "AddComponent failed: ", added.Error().Message());
    return **added;
}

/// Moves a node, failing the test case on refusal.
void Move(Scene& scene, ecs::Entity node, const Transform& local) {
    REQUIRE(scene.SetLocalTransform(node, local).HasValue());
}

/// Finds a node by name, or fails the test case.
[[nodiscard]] ecs::Entity FindByName(const Scene& scene, std::string_view name) {
    for (const ecs::Entity entity : scene.Nodes()) {
        const NameComponent* component = scene.World().Get<NameComponent>(entity);
        if (component != nullptr && component->name == name) {
            return entity;
        }
    }
    CHECK_MESSAGE(false, "No node named '", name, "' in the scene");
    return ecs::kNullEntity;
}

[[nodiscard]] assets::MeshData MakeQuad(const std::string& name) {
    assets::MeshData mesh;
    mesh.name = name;
    mesh.positions = {Vec3{-1, -1, 0}, Vec3{1, -1, 0}, Vec3{1, 1, 0}, Vec3{-1, 1, 0}};
    mesh.normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    mesh.uvs = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.bounds.min = Vec3{-1, -1, 0};
    mesh.bounds.max = Vec3{1, 1, 0};
    return mesh;
}

[[nodiscard]] std::vector<u8> FakeShader() { return std::vector<u8>(64, 0x42); }

[[nodiscard]] render::ShaderLibrary MakeShaderLibrary() {
    render::ShaderLibrary library;
    library.forwardVertex = FakeShader();
    library.forwardFragment = FakeShader();
    library.shadowVertex = FakeShader();
    library.shadowFragment = FakeShader();
    library.bloomDownsample = FakeShader();
    library.bloomUpsample = FakeShader();
    library.ssaoCompute = FakeShader();
    library.tonemapVertex = FakeShader();
    library.tonemapFragment = FakeShader();
    return library;
}

[[nodiscard]] ConstByteSpan BytesOf(const std::vector<u8>& data) {
    return std::as_bytes(std::span(data.data(), data.size()));
}

[[nodiscard]] ConstByteSpan BytesOf(const std::string& text) {
    return std::as_bytes(std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

/// Cooked assets on a memory file system plus the runtime loader, a null device,
/// a renderer and the resource cache: everything the demo needs to draw.
class CookedProject {
public:
    CookedProject() {
        assets::MeshDocument document;
        document.name = "quad";
        document.meshes.push_back(MakeQuad("quad"));

        meshId_ = assets::AssetId::Generate();
        auto cooked = assets::CookMeshDocument(document, 0x1234, 0x5678);
        REQUIRE(cooked.HasValue());
        const assets::AssetPath cookedPath{assets::CookedFileName(meshId_, assets::AssetType::Mesh)};
        REQUIRE(cooked_.WriteFile(cookedPath.Text(), BytesOf(*cooked)).HasValue());

        assets::CookedManifest manifest;
        assets::CookedManifestEntry entry;
        entry.id = meshId_;
        entry.sourcePath = assets::AssetPath{"models/quad.glb"};
        entry.cookedPath = cookedPath;
        entry.type = assets::AssetType::Mesh;
        entry.name = "quad";
        manifest.entries.push_back(std::move(entry));
        manifest.Sort();
        REQUIRE(cooked_.WriteFile(assets::kManifestFileName, BytesOf(manifest.Dump())).HasValue());

        REQUIRE(manager_.Initialize(cooked_).HasValue());

        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        desc.enableValidation = true;
        auto device = rhi::CreateDevice(desc);
        REQUIRE_MESSAGE(device.HasValue(), "Failed to create the null device");
        device_ = std::move(*device);

        render::RendererSettings settings;
        settings.enableSsao = false;
        settings.enableBloom = false;
        renderer_ = std::make_unique<render::Renderer>(settings);
        REQUIRE(renderer_->Initialize(*device_, MakeShaderLibrary()).HasValue());

        auto commands = device_->CreateCommandBuffer();
        REQUIRE(commands.HasValue());
        commands_ = std::move(*commands);
    }

    [[nodiscard]] assets::AssetId MeshId() const noexcept { return meshId_; }
    [[nodiscard]] rhi::IDevice& Device() { return *device_; }
    [[nodiscard]] render::Renderer& Render() { return *renderer_; }
    [[nodiscard]] rhi::ICommandBuffer& Commands() { return *commands_; }
    [[nodiscard]] assets::AssetManager& Manager() { return manager_; }
    [[nodiscard]] std::vector<std::string> RhiErrors() const {
        return device_->ValidationErrors();
    }

private:
    assets::MemoryFileSystem cooked_;
    assets::AssetManager manager_;
    assets::AssetId meshId_;
    std::unique_ptr<rhi::IDevice> device_;
    std::unique_ptr<render::Renderer> renderer_;
    rhi::CommandBufferPtr commands_;
};

} // namespace

// ===========================================================================
TEST_SUITE("scene.hierarchy") {
    TEST_CASE("nodes are created with a parent link and a name") {
        Scene scene("Level");
        CHECK(scene.Name() == "Level");
        CHECK(scene.NodeCount() == 0);

        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);

        CHECK(scene.IsNode(root));
        CHECK(scene.IsNode(child));
        CHECK_FALSE(scene.IsNode(ecs::kNullEntity));
        CHECK(scene.NodeCount() == 2);
        CHECK(scene.Parent(child) == root);
        CHECK_FALSE(scene.Parent(root).IsValid());
        CHECK(scene.World().Get<NameComponent>(child)->name == "Child");
        REQUIRE(scene.Children(root).size() == 1);
        CHECK(scene.Children(root)[0] == child);
        CHECK(scene.Children(child).empty());
    }

    TEST_CASE("creating a node under something that is not a node is an error") {
        Scene scene;
        const ecs::Entity stranger = scene.World().CreateEntity();
        auto created = scene.CreateNode("Orphan", stranger);
        REQUIRE(created.IsError());
        CHECK(created.Error().Code() == StatusCode::InvalidArgument);
        CHECK(scene.NodeCount() == 0);
    }

    TEST_CASE("children come back in creation order") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity first = Node(scene, "first", root);
        const ecs::Entity second = Node(scene, "second", root);
        const ecs::Entity third = Node(scene, "third", root);

        REQUIRE(scene.Children(root).size() == 3);
        CHECK(scene.Children(root)[0] == first);
        CHECK(scene.Children(root)[1] == second);
        CHECK(scene.Children(root)[2] == third);
    }

    TEST_CASE("reparenting moves a node and its whole subtree") {
        Scene scene;
        const ecs::Entity a = Node(scene, "A");
        const ecs::Entity b = Node(scene, "B");
        const ecs::Entity child = Node(scene, "child", a);
        const ecs::Entity grandchild = Node(scene, "grandchild", child);

        REQUIRE(scene.SetParent(child, b).HasValue());
        CHECK(scene.Parent(child) == b);
        CHECK(scene.Children(a).empty());
        REQUIRE(scene.Children(b).size() == 1);
        // The subtree travels with the node.
        CHECK(scene.Parent(grandchild) == child);
        CHECK(scene.IsDescendant(grandchild, b));
        CHECK_FALSE(scene.IsDescendant(grandchild, a));
        CHECK(scene.Depth(grandchild) == 2);
    }

    TEST_CASE("reparenting to no parent makes a node a root") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);

        REQUIRE(scene.SetParent(child, ecs::kNullEntity).HasValue());
        CHECK_FALSE(scene.Parent(child).IsValid());
        CHECK(scene.Children(root).empty());
        CHECK(scene.Roots().size() == 2);
    }

    TEST_CASE("cycles are rejected") {
        Scene scene;
        const ecs::Entity a = Node(scene, "A");
        const ecs::Entity b = Node(scene, "B", a);
        const ecs::Entity c = Node(scene, "C", b);

        auto selfParent = scene.SetParent(a, a);
        REQUIRE(selfParent.IsError());
        CHECK(selfParent.Error().Code() == StatusCode::InvalidArgument);

        // A under its own grandchild would close the loop.
        auto cycle = scene.SetParent(a, c);
        REQUIRE(cycle.IsError());
        CHECK(cycle.Error().Code() == StatusCode::InvalidArgument);

        // Nothing moved.
        CHECK(scene.Parent(b) == a);
        CHECK(scene.Parent(c) == b);
        CHECK(scene.Depth(c) == 2);
    }

    TEST_CASE("reparenting to a dead entity is an error") {
        Scene scene;
        const ecs::Entity a = Node(scene, "A");
        const ecs::Entity b = Node(scene, "B");
        scene.DestroyNode(b);

        const auto result = scene.SetParent(a, b);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("destroying a node destroys its subtree") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);
        const ecs::Entity grandchild = Node(scene, "Grandchild", child);
        const ecs::Entity sibling = Node(scene, "Sibling", root);

        scene.DestroyNode(child);

        CHECK_FALSE(scene.IsNode(child));
        CHECK_FALSE(scene.IsNode(grandchild));
        CHECK(scene.IsNode(sibling));
        CHECK(scene.NodeCount() == 2);
        REQUIRE(scene.Children(root).size() == 1);
        CHECK(scene.Children(root)[0] == sibling);
    }

    TEST_CASE("a stale handle does not alias a recycled entity") {
        Scene scene;
        const ecs::Entity first = Node(scene, "First");
        scene.DestroyNode(first);
        CHECK_FALSE(scene.IsNode(first));

        const ecs::Entity second = Node(scene, "Second");
        // The index is reused, the generation is not, so the old handle stays dead.
        CHECK(second.index == first.index);
        CHECK_FALSE(scene.IsNode(first));
        CHECK(scene.IsNode(second));
        CHECK(scene.World().Get<NameComponent>(second)->name == "Second");
    }

    TEST_CASE("a node whose parent was destroyed through the world stays traversable") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);

        // Bypassing the scene graph is not supported, but it must not wedge the
        // traversal: the orphan is reported as a root instead of hanging.
        scene.World().DestroyEntity(root);

        CHECK_FALSE(scene.Parent(child).IsValid());
        const std::vector<ecs::Entity> roots = scene.Roots();
        REQUIRE(roots.size() == 1);
        CHECK(roots[0] == child);
        CHECK(scene.Depth(child) == 0);
        CHECK(scene.UpdateTransforms() == 1);
    }

    TEST_CASE("adding a component to a non node is an error") {
        Scene scene;
        const ecs::Entity stranger = scene.World().CreateEntity();
        auto added = scene.AddComponent<LightComponent>(stranger);
        REQUIRE(added.IsError());
        CHECK(added.Error().Code() == StatusCode::InvalidArgument);

        const ecs::Entity node = Node(scene, "Light");
        CHECK(Add<LightComponent>(scene, node).type == LightComponent::Type::Directional);
    }

    TEST_CASE("roots and nodes are reported in entity index order") {
        Scene scene;
        const ecs::Entity a = Node(scene, "A");
        const ecs::Entity b = Node(scene, "B");
        const ecs::Entity childOfA = Node(scene, "A.child", a);

        const std::vector<ecs::Entity> roots = scene.Roots();
        REQUIRE(roots.size() == 2);
        CHECK(roots[0] == a);
        CHECK(roots[1] == b);

        const std::vector<ecs::Entity> nodes = scene.Nodes();
        REQUIRE(nodes.size() == 3);
        CHECK(nodes[0] == a);
        CHECK(nodes[1] == b);
        CHECK(nodes[2] == childOfA);
    }

    TEST_CASE("Clear empties the scene but keeps its name") {
        Scene scene("Level");
        const ecs::Entity root = Node(scene, "Root");
        Node(scene, "Child", root);
        const ecs::Entity camera = Node(scene, "Camera");
        Add<CameraComponent>(scene, camera);
        REQUIRE(scene.SetActiveCamera(camera).HasValue());

        scene.Clear();
        CHECK(scene.NodeCount() == 0);
        CHECK(scene.Roots().empty());
        CHECK_FALSE(scene.ActiveCamera().IsValid());
        CHECK_FALSE(scene.HasPendingUpdates());
        CHECK(scene.Name() == "Level");
    }
}

// ===========================================================================
TEST_SUITE("scene.transforms") {
    TEST_CASE("world matrices compose down the hierarchy") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);
        const ecs::Entity grandchild = Node(scene, "Grandchild", child);

        Move(scene, root, Transform{Vec3{10, 0, 0}, Quaternion::Identity(), Vec3::One()});
        Move(scene, child, Transform{Vec3{0, 5, 0}, Quaternion::Identity(), Vec3::One()});
        Move(scene, grandchild, Transform{Vec3{0, 0, 2}, Quaternion::Identity(), Vec3::One()});

        CHECK(scene.UpdateTransforms() == 3);

        CHECK(TranslationOf(scene.WorldMatrix(root)) == Vec3{10, 0, 0});
        CHECK(TranslationOf(scene.WorldMatrix(child)) == Vec3{10, 5, 0});
        CHECK(TranslationOf(scene.WorldMatrix(grandchild)) == Vec3{10, 5, 2});
        CHECK(scene.WorldPosition(grandchild) == Vec3{10, 5, 2});
        CHECK_FALSE(scene.HasPendingUpdates());
        CHECK(scene.DirtyNodeCount() == 0);
    }

    TEST_CASE("a static scene costs one pass and then nothing") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);
        Node(scene, "Grandchild", child);

        CHECK(scene.UpdateTransforms() == 3);
        CHECK(scene.UpdateTransforms() == 0);
        CHECK(scene.UpdateTransforms() == 0);
    }

    TEST_CASE("moving a parent moves its children") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);
        Move(scene, child, Transform{Vec3{1, 0, 0}, Quaternion::Identity(), Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 2);

        Move(scene, root, Transform{Vec3{100, 0, 0}, Quaternion::Identity(), Vec3::One()});
        // The parent and its subtree, and nothing else: the dirty walk is the
        // whole point of the cache.
        CHECK(scene.DirtyNodeCount() == 2);
        CHECK(scene.UpdateTransforms() == 2);
        CHECK(scene.WorldPosition(child) == Vec3{101, 0, 0});
    }

    TEST_CASE("moving a leaf does not disturb its siblings") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity left = Node(scene, "Left", root);
        const ecs::Entity right = Node(scene, "Right", root);
        Move(scene, left, Transform{Vec3{-1, 0, 0}, Quaternion::Identity(), Vec3::One()});
        Move(scene, right, Transform{Vec3{1, 0, 0}, Quaternion::Identity(), Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 3);

        Move(scene, left, Transform{Vec3{-5, 0, 0}, Quaternion::Identity(), Vec3::One()});
        // Root and left are dirty; right is not, and the update skips it.
        CHECK(scene.DirtyNodeCount() == 2);
        CHECK(scene.UpdateTransforms() == 2);
        CHECK(scene.WorldPosition(left) == Vec3{-5, 0, 0});
        CHECK(scene.WorldPosition(right) == Vec3{1, 0, 0});
    }

    TEST_CASE("a dirty leaf under a clean parent is still updated") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);
        const ecs::Entity leaf = Node(scene, "Leaf", child);
        REQUIRE(scene.UpdateTransforms() == 3);

        // The invariant that makes pruning sound: dirtying a node dirties the
        // path up to the root, so a top down walk can never step over a dirty
        // node into a clean one.
        Move(scene, leaf, Transform{Vec3{7, 0, 0}, Quaternion::Identity(), Vec3::One()});
        CHECK(scene.DirtyNodeCount() == 3);
        CHECK(scene.UpdateTransforms() == 3);
        CHECK(scene.WorldPosition(leaf) == Vec3{7, 0, 0});
    }

    TEST_CASE("scale and rotation are inherited") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        const ecs::Entity child = Node(scene, "Child", root);

        Move(scene, root, Transform{Vec3::Zero(), Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi),
                                   Vec3{2, 2, 2}});
        Move(scene, child, Transform{Vec3{1, 0, 0}, Quaternion::Identity(), Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 2);

        // A 90 degree yaw takes +X to -Z, and the parent's scale of 2 doubles it.
        const Vec3 position = scene.WorldPosition(child);
        CHECK(position.x == doctest::Approx(0.0f).epsilon(1e-5f));
        CHECK(position.y == doctest::Approx(0.0f));
        CHECK(position.z == doctest::Approx(-2.0f).epsilon(1e-5f));
    }

    TEST_CASE("a node created under a parent starts at the parent's position") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Root");
        Move(scene, root, Transform{Vec3{5, 6, 7}, Quaternion::Identity(), Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 1);

        // No update in between: the node must not flash at the origin.
        const ecs::Entity child = Node(scene, "Child", root);
        CHECK(scene.WorldPosition(child) == Vec3{5, 6, 7});
        CHECK(scene.HasPendingUpdates());
        // Two, not one: invalidating a node also invalidates the path to the
        // root, which is what lets the update prune clean subtrees.  The parent
        // recomputes to the same matrix, so the cost is one multiply per level.
        CHECK(scene.UpdateTransforms() == 2);
        CHECK(scene.WorldPosition(child) == Vec3{5, 6, 7});
    }

    TEST_CASE("reparenting invalidates the moved subtree") {
        Scene scene;
        const ecs::Entity a = Node(scene, "A");
        const ecs::Entity b = Node(scene, "B");
        const ecs::Entity child = Node(scene, "Child", a);
        Move(scene, a, Transform{Vec3{10, 0, 0}, Quaternion::Identity(), Vec3::One()});
        Move(scene, b, Transform{Vec3{-10, 0, 0}, Quaternion::Identity(), Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 3);
        CHECK(scene.WorldPosition(child) == Vec3{10, 0, 0});

        REQUIRE(scene.SetParent(child, b).HasValue());
        CHECK(scene.HasPendingUpdates());
        // The moved node and its new parent (the path to the root).
        CHECK(scene.UpdateTransforms() == 2);
        CHECK(scene.WorldPosition(child) == Vec3{-10, 0, 0});
    }

    TEST_CASE("world matrices of unknown entities are the identity") {
        Scene scene;
        const Mat4& matrix = scene.WorldMatrix(ecs::kNullEntity);
        CHECK(matrix == Mat4::Identity());
        CHECK(scene.LocalTransform(ecs::kNullEntity) == nullptr);
        CHECK(scene.WorldPosition(ecs::kNullEntity) == Vec3::Zero());
    }

    TEST_CASE("transforms on a non node are an error") {
        Scene scene;
        const auto result = scene.SetLocalTransform(ecs::kNullEntity, Transform{});
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
    }
}

// ===========================================================================
TEST_SUITE("scene.draw_items") {
    TEST_CASE("renderers become draw items with their world matrix") {
        Scene scene;
        const assets::AssetId meshId = assets::AssetId::Generate();
        FakeResolver resolver;
        resolver.Add(meshId, 3, Box(1.0f));

        const ecs::Entity root = Node(scene, "Root");
        Move(scene, root, Transform{Vec3{4, 0, 0}, Quaternion::Identity(), Vec3::One()});
        const ecs::Entity node = Node(scene, "Prop", root);
        MeshRendererComponent& renderer = Add<MeshRendererComponent>(scene, node);
        renderer.lods[0] = meshId;
        renderer.tint = Vec4{0.5f, 0.5f, 0.5f, 1.0f};
        REQUIRE(scene.UpdateTransforms() == 2);

        std::vector<render::DrawItem> items;
        std::vector<ecs::Entity> sources;
        u32 unresolved = 0;
        scene.CollectDrawItems(resolver, items, sources, unresolved);

        REQUIRE(items.size() == 1);
        REQUIRE(sources.size() == 1);
        CHECK(sources[0] == node);
        CHECK(unresolved == 0);
        CHECK(items[0].lods[0] == 3);
        CHECK(items[0].tint == Vec4{0.5f, 0.5f, 0.5f, 1.0f});
        CHECK(TranslationOf(items[0].world) == Vec3{4, 0, 0});
        CHECK(items[0].localBounds.min == Vec3{-1, -1, -1});
        CHECK(items[0].castsShadow);
    }

    TEST_CASE("hidden and meshless nodes are not drawn") {
        Scene scene;
        const assets::AssetId meshId = assets::AssetId::Generate();
        FakeResolver resolver;
        resolver.Add(meshId, 1, Box(1.0f));

        const ecs::Entity visible = Node(scene, "Visible");
        Add<MeshRendererComponent>(scene, visible).lods[0] = meshId;

        const ecs::Entity hidden = Node(scene, "Hidden");
        MeshRendererComponent& hiddenRenderer = Add<MeshRendererComponent>(scene, hidden);
        hiddenRenderer.lods[0] = meshId;
        hiddenRenderer.visible = false;

        // A node with a renderer but no mesh assigned.
        const ecs::Entity empty = Node(scene, "Empty");
        Add<MeshRendererComponent>(scene, empty);

        std::vector<render::DrawItem> items;
        std::vector<ecs::Entity> sources;
        u32 unresolved = 0;
        scene.CollectDrawItems(resolver, items, sources, unresolved);

        REQUIRE(items.size() == 1);
        CHECK(sources[0] == visible);
        CHECK(unresolved == 0);
    }

    TEST_CASE("unresolved assets are reported, not fatal") {
        Scene scene;
        const assets::AssetId known = assets::AssetId::Generate();
        const assets::AssetId missing = assets::AssetId::Generate();
        FakeResolver resolver;
        resolver.Add(known, 7, Box(1.0f));

        const ecs::Entity partial = Node(scene, "Partial");
        MeshRendererComponent& partialRenderer = Add<MeshRendererComponent>(scene, partial);
        partialRenderer.lods[0] = known;
        partialRenderer.lods[1] = missing;

        const ecs::Entity hopeless = Node(scene, "Hopeless");
        Add<MeshRendererComponent>(scene, hopeless).lods[0] = missing;

        std::vector<render::DrawItem> items;
        std::vector<ecs::Entity> sources;
        u32 unresolved = 0;
        scene.CollectDrawItems(resolver, items, sources, unresolved);

        // The partially resolved node still draws; the other one does not.
        REQUIRE(items.size() == 1);
        CHECK(sources[0] == partial);
        CHECK(items[0].lods[0] == 7);
        CHECK(items[0].lods[1] == render::kInvalidMesh);
        CHECK(unresolved == 2);
    }

    TEST_CASE("output is sorted by entity, whatever the insertion order") {
        Scene scene;
        const assets::AssetId meshId = assets::AssetId::Generate();
        FakeResolver resolver;
        resolver.Add(meshId, 1, Box(1.0f));

        for (int i = 0; i < 5; ++i) {
            const ecs::Entity node = Node(scene, "Node");
            Add<MeshRendererComponent>(scene, node).lods[0] = meshId;
        }

        std::vector<render::DrawItem> items;
        std::vector<ecs::Entity> sources;
        u32 unresolved = 0;
        scene.CollectDrawItems(resolver, items, sources, unresolved);

        REQUIRE(sources.size() == 5);
        for (usize i = 1; i < sources.size(); ++i) {
            CHECK(sources[i - 1].index < sources[i].index);
        }
        // And it is stable: the same scene gives the same order twice.
        std::vector<ecs::Entity> again;
        scene.CollectDrawItems(resolver, items, again, unresolved);
        CHECK(again == sources);
    }

    TEST_CASE("lod bounds are unioned so a coarse lod is never culled early") {
        Scene scene;
        const assets::AssetId fine = assets::AssetId::Generate();
        const assets::AssetId coarse = assets::AssetId::Generate();
        FakeResolver resolver;
        resolver.Add(fine, 1, Box(1.0f));
        resolver.Add(coarse, 2, Box(4.0f));

        const ecs::Entity node = Node(scene, "Lodded");
        MeshRendererComponent& renderer = Add<MeshRendererComponent>(scene, node);
        renderer.lods[0] = fine;
        renderer.lods[1] = coarse;

        std::vector<render::DrawItem> items;
        std::vector<ecs::Entity> sources;
        u32 unresolved = 0;
        scene.CollectDrawItems(resolver, items, sources, unresolved);

        REQUIRE(items.size() == 1);
        CHECK(items[0].localBounds.min == Vec3{-4, -4, -4});
        CHECK(items[0].localBounds.max == Vec3{4, 4, 4});
    }

    TEST_CASE("the collected items feed the renderer end to end") {
        CookedProject project;
        SceneResources resources;

        auto handle =
            resources.Mesh(project.Device(), project.Render(), project.Manager(), project.MeshId());
        REQUIRE_MESSAGE(handle.HasValue(), "Mesh upload failed: ", handle.Error().Message());
        CHECK(resources.MeshCount() == 1);
        CHECK(resources.IsMeshLoaded(project.MeshId()));
        // Loading twice must not upload twice.
        auto again =
            resources.Mesh(project.Device(), project.Render(), project.Manager(), project.MeshId());
        REQUIRE(again.HasValue());
        CHECK(*again == *handle);
        CHECK(project.Render().MeshCount() == 1);

        // An unknown id is an error, not a silent invalid handle.
        auto unknown = resources.Mesh(project.Device(), project.Render(), project.Manager(),
                                      assets::AssetId::Generate());
        CHECK(unknown.IsError());
        CHECK(resources.Resolve(assets::AssetId::Generate()).IsValid() == false);

        Scene scene("Demo");
        const ecs::Entity prop = Node(scene, "Prop");
        Add<MeshRendererComponent>(scene, prop).lods[0] = project.MeshId();
        const ecs::Entity sun = Node(scene, "Sun");
        Add<LightComponent>(scene, sun).intensity = 2.0f;
        REQUIRE(scene.UpdateTransforms() == 2);

        std::vector<render::DrawItem> items;
        std::vector<ecs::Entity> sources;
        u32 unresolved = 0;
        scene.CollectDrawItems(resources, items, sources, unresolved);
        REQUIRE(items.size() == 1);
        CHECK(unresolved == 0);
        // The bounds travelled from the cooked file to the draw item.
        CHECK(items[0].localBounds.min == Vec3{-1, -1, 0});

        const auto sunLight = scene.FindSunLight();
        REQUIRE(sunLight.has_value());

        render::FrameView frameView;
        frameView.position = Vec3{0, 0, 5};
        frameView.target = Vec3{0, 0, 0};
        frameView.aspect = 800.0f / 600.0f;
        frameView.viewportWidth = 800;
        frameView.viewportHeight = 600;
        frameView.Update();

        project.Commands().Begin();
        const auto rendered = project.Render().RenderFrame(project.Device(), project.Commands(),
                                                          frameView, items, *sunLight, {});
        project.Commands().End();
        for (const std::string& error : project.RhiErrors()) {
            MESSAGE("rhi: ", error);
        }
        REQUIRE_MESSAGE(rendered.HasValue(), "RenderFrame failed: ", rendered.Error().Message());

        const render::RendererStats& stats = project.Render().Stats();
        MESSAGE("items=", stats.drawItems, " visible=", stats.visibleItems, " batches=",
                stats.batches, " draws=", stats.drawCalls);
        CHECK(stats.drawItems == 1);
        CHECK(stats.visibleItems == 1);
        CHECK(stats.batches == 1);
        CHECK(stats.instances == 1);
        CHECK(project.RhiErrors().empty());
    }
}

// ===========================================================================
TEST_SUITE("scene.lights") {
    TEST_CASE("the sun is the first directional light in node order") {
        Scene scene;
        const ecs::Entity point = Node(scene, "Point");
        Add<LightComponent>(scene, point).type = LightComponent::Type::Point;

        const ecs::Entity second = Node(scene, "SecondSun");
        Add<LightComponent>(scene, second);

        const ecs::Entity first = Node(scene, "FirstSun");
        Add<LightComponent>(scene, first).intensity = 3.0f;

        const auto sun = scene.FindSunLight();
        REQUIRE(sun.has_value());
        // "Point" comes first in node order but is not directional, so the
        // second node wins over the third.
        CHECK(sun->intensity == doctest::Approx(1.0f));
    }

    TEST_CASE("a scene with no light has no sun") {
        Scene scene;
        Node(scene, "Empty");
        CHECK_FALSE(scene.FindSunLight().has_value());
    }

    TEST_CASE("the light direction comes from the node orientation") {
        Scene scene;
        const ecs::Entity sun = Node(scene, "Sun");
        Add<LightComponent>(scene, sun);

        // Pitch 90 degrees down: a negative angle about +X takes the node's -Z
        // axis from horizontal to straight at the ground.
        Move(scene, sun,
             Transform{Vec3::Zero(), Quaternion::FromAxisAngle(Vec3::Right(), -kHalfPi),
                       Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 1);

        const auto light = scene.FindSunLight();
        REQUIRE(light.has_value());
        CHECK(light->direction.x == doctest::Approx(0.0f).epsilon(1e-5f));
        CHECK(light->direction.y == doctest::Approx(-1.0f).epsilon(1e-5f));
        CHECK(light->direction.z == doctest::Approx(0.0f).epsilon(1e-5f));
    }

    TEST_CASE("lights are collected in node order with their world position") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Rig");
        Move(scene, root, Transform{Vec3{1, 2, 3}, Quaternion::Identity(), Vec3::One()});
        const ecs::Entity lamp = Node(scene, "Lamp", root);
        Add<LightComponent>(scene, lamp).type = LightComponent::Type::Point;
        REQUIRE(scene.UpdateTransforms() == 2);

        const std::vector<Scene::SceneLight> lights = scene.CollectLights();
        REQUIRE(lights.size() == 1);
        CHECK(lights[0].entity == lamp);
        CHECK(lights[0].light.type == LightComponent::Type::Point);
        CHECK(lights[0].position == Vec3{1, 2, 3});
    }
}

// ===========================================================================
TEST_SUITE("scene.camera") {
    TEST_CASE("a camera node builds the frame view") {
        Scene scene;
        const ecs::Entity root = Node(scene, "Rig");
        Move(scene, root, Transform{Vec3{0, 0, 10}, Quaternion::Identity(), Vec3::One()});
        const ecs::Entity camera = Node(scene, "Camera", root);
        CameraComponent& component = Add<CameraComponent>(scene, camera);
        component.fovYDegrees = 90.0f;
        component.farPlane = 1000.0f;
        REQUIRE(scene.UpdateTransforms() == 2);

        auto view = scene.BuildFrameView(camera, 1600, 800);
        REQUIRE_MESSAGE(view.HasValue(), view.Error().Message());
        CHECK(view->position == Vec3{0, 0, 10});
        // The node's -Z axis is the view direction.
        CHECK(view->target == Vec3{0, 0, 9});
        CHECK(view->fovYDegrees == doctest::Approx(90.0f));
        CHECK(view->aspect == doctest::Approx(2.0f));
        CHECK(view->farPlane == doctest::Approx(1000.0f));
        CHECK(view->frustum.ContainsPoint(Vec3{0, 0, 0}));
    }

    TEST_CASE("a top down camera gets a finite view matrix") {
        Scene scene;
        const ecs::Entity camera = Node(scene, "TopDown");
        Add<CameraComponent>(scene, camera);
        Move(scene, camera, Transform{Vec3{0, 20, 0},
                                     Quaternion::FromAxisAngle(Vec3::Right(), -kHalfPi),
                                     Vec3::One()});
        REQUIRE(scene.UpdateTransforms() == 1);

        auto view = scene.BuildFrameView(camera, 800, 600);
        REQUIRE(view.HasValue());
        CHECK(IsFinite(view->view));
        CHECK(IsFinite(view->viewProjection));
        // The ground below the camera must be inside the frustum.
        CHECK(view->frustum.ContainsPoint(Vec3{0, 0, 0}));
    }

    TEST_CASE("a view whose up vector is parallel to its direction does not collapse") {
        // LookAtRH builds its basis from Cross(up, forward), which is the zero
        // vector here: a straight down view with the world up as its up vector.
        // This is the case a transform derived camera cannot produce (a rotation
        // keeps its axes orthogonal), which is exactly why an editor's free look
        // or a look-at helper has to be covered on its own.
        render::FrameView view;
        view.position = Vec3{0, 20, 0};
        view.target = Vec3{0, 0, 0};
        view.up = Vec3::Up();
        view.viewportWidth = 800;
        view.viewportHeight = 600;
        view.Update();

        CHECK(IsFinite(view.view));
        const Vec3 right{view.view.At(0, 0), view.view.At(0, 1), view.view.At(0, 2)};
        const Vec3 up{view.view.At(1, 0), view.view.At(1, 1), view.view.At(1, 2)};
        CHECK(Length(right) == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(Length(up) == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(view.frustum.ContainsPoint(Vec3{0, 0, 0}));
    }

    TEST_CASE("the substituted up vector is deterministic for one direction") {
        render::FrameView down;
        down.position = Vec3{0, 10, 0};
        down.target = Vec3{0, 0, 0};
        down.up = Vec3::Up();
        down.Update();

        render::FrameView other = down;
        other.Update();
        CHECK(down.view == other.view);

        // Looking straight up takes the same path with a different substitute.
        render::FrameView up;
        up.position = Vec3{0, -10, 0};
        up.target = Vec3{0, 0, 0};
        up.up = Vec3::Up();
        up.Update();
        CHECK(IsFinite(up.view));
        CHECK(Length(Vec3{up.view.At(0, 0), up.view.At(0, 1), up.view.At(0, 2)}) ==
              doctest::Approx(1.0f).epsilon(1e-5f));
    }

    TEST_CASE("a camera sitting on its own target does not degenerate") {
        render::FrameView view;
        view.position = Vec3{1, 2, 3};
        view.target = Vec3{1, 2, 3};
        view.Update();
        CHECK(IsFinite(view.view));
        CHECK(view.frustum.ContainsPoint(Vec3{1, 2, -5}));
    }

    TEST_CASE("an invalid camera is rejected instead of producing NaN") {
        Scene scene;
        const ecs::Entity node = Node(scene, "Broken");
        CameraComponent& camera = Add<CameraComponent>(scene, node);
        camera.nearPlane = 10.0f;
        camera.farPlane = 1.0f; // Far inside near.
        REQUIRE(scene.UpdateTransforms() == 1);

        const auto view = scene.BuildFrameView(node, 800, 600);
        REQUIRE(view.IsError());
        CHECK(view.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("a zero sized viewport is rejected") {
        Scene scene;
        const ecs::Entity node = Node(scene, "Camera");
        Add<CameraComponent>(scene, node);
        REQUIRE(scene.UpdateTransforms() == 1);
        CHECK(scene.BuildFrameView(node, 0, 600).IsError());
        CHECK(scene.BuildFrameView(node, 800, 0).IsError());
    }

    TEST_CASE("a non camera node is rejected") {
        Scene scene;
        const ecs::Entity node = Node(scene, "Prop");
        const auto view = scene.BuildFrameView(node, 800, 600);
        REQUIRE(view.IsError());
        CHECK(view.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("only one camera is active at a time") {
        Scene scene;
        const ecs::Entity first = Node(scene, "First");
        Add<CameraComponent>(scene, first);
        const ecs::Entity second = Node(scene, "Second");
        Add<CameraComponent>(scene, second);
        const ecs::Entity prop = Node(scene, "Prop");

        // Note the components are re-read rather than held: adding the second
        // camera can reallocate the pool, which is the lifetime rule the Scene
        // header states.  Holding a reference across a structural change reads
        // freed memory, and the sanitizer build says so.
        CHECK_FALSE(scene.ActiveCamera().IsValid());
        CHECK(scene.SetActiveCamera(prop).IsError());

        REQUIRE(scene.SetActiveCamera(first).HasValue());
        CHECK(scene.ActiveCamera() == first);
        CHECK(scene.World().Get<CameraComponent>(first)->active);

        REQUIRE(scene.SetActiveCamera(second).HasValue());
        CHECK(scene.ActiveCamera() == second);
        CHECK_FALSE(scene.World().Get<CameraComponent>(first)->active);
        CHECK(scene.World().Get<CameraComponent>(second)->active);

        // Destroying the active camera clears it rather than leaving a dangling
        // handle behind.
        scene.DestroyNode(second);
        CHECK_FALSE(scene.ActiveCamera().IsValid());
    }
}

// ===========================================================================
TEST_SUITE("scene.serialization") {
    /// A scene worth round tripping: a nested hierarchy, every component type,
    /// an active camera and a light.
    [[nodiscard]] Scene MakeScene() {
        Scene scene("RoundTrip");
        const ecs::Entity root = Node(scene, "Root");
        Move(scene, root, Transform{Vec3{1, 2, 3}, Quaternion::Identity(), Vec3::One()});

        const ecs::Entity prop = Node(scene, "Prop", root);
        Move(scene, prop, Transform{Vec3{0, 1, 0}, Quaternion::FromAxisAngle(Vec3::Up(), 0.5f),
                                   Vec3{2, 2, 2}});
        MeshRendererComponent& renderer = Add<MeshRendererComponent>(scene, prop);
        renderer.lods[0] = assets::AssetId::FromName("models/quad");
        renderer.lods[1] = assets::AssetId::FromName("models/quad-lod1");
        renderer.material = assets::AssetId::FromName("materials/steel");
        renderer.tint = Vec4{0.25f, 0.5f, 0.75f, 1.0f};
        renderer.castsShadow = false;

        const ecs::Entity sun = Node(scene, "Sun");
        Move(scene, sun,
             Transform{Vec3::Zero(), Quaternion::FromAxisAngle(Vec3::Right(), 1.0f), Vec3::One()});
        LightComponent& light = Add<LightComponent>(scene, sun);
        light.intensity = 4.0f;
        light.color = Vec3{1.0f, 0.9f, 0.8f};

        const ecs::Entity camera = Node(scene, "Camera", root);
        CameraComponent& cameraComponent = Add<CameraComponent>(scene, camera);
        cameraComponent.fovYDegrees = 75.0f;
        cameraComponent.farPlane = 900.0f;
        REQUIRE(scene.SetActiveCamera(camera).HasValue());
        return scene;
    }

    TEST_CASE("a scene survives a round trip") {
        const Scene original = MakeScene();
        auto text = SceneSerializer::ToJson(original);
        REQUIRE(text.HasValue());

        Scene loaded;
        REQUIRE(SceneSerializer::FromJson(*text, loaded).HasValue());

        CHECK(loaded.Name() == "RoundTrip");
        CHECK(loaded.NodeCount() == original.NodeCount());
        CHECK(loaded.NodeCount() == 4);

        const ecs::Entity prop = FindByName(loaded, "Prop");
        const ecs::Entity camera = FindByName(loaded, "Camera");
        CHECK(FindByName(loaded, "Root") == loaded.Parent(prop));
        CHECK(loaded.Parent(camera) == loaded.Parent(prop));

        const Transform* local = loaded.LocalTransform(prop);
        REQUIRE(local != nullptr);
        CHECK(local->position == Vec3{0, 1, 0});
        CHECK(local->scale == Vec3{2, 2, 2});
        CHECK(local->rotation.y == doctest::Approx(std::sin(0.25f)).epsilon(1e-6f));

        const auto* renderer = loaded.World().Get<MeshRendererComponent>(prop);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->lods[0] == assets::AssetId::FromName("models/quad"));
        CHECK(renderer->lods[1] == assets::AssetId::FromName("models/quad-lod1"));
        CHECK(renderer->material == assets::AssetId::FromName("materials/steel"));
        CHECK(renderer->tint == Vec4{0.25f, 0.5f, 0.75f, 1.0f});
        CHECK_FALSE(renderer->castsShadow);

        const auto* sun = loaded.World().Get<LightComponent>(FindByName(loaded, "Sun"));
        REQUIRE(sun != nullptr);
        CHECK(sun->intensity == doctest::Approx(4.0f));
        CHECK(sun->color == Vec3{1.0f, 0.9f, 0.8f});

        // The active camera and the world matrices.
        CHECK(loaded.ActiveCamera() == camera);
        CHECK(loaded.World().Get<CameraComponent>(camera)->active);
        CHECK(loaded.World().Get<CameraComponent>(camera)->fovYDegrees == doctest::Approx(75.0f));
        CHECK_FALSE(loaded.HasPendingUpdates());
        CHECK(loaded.WorldPosition(prop) == Vec3{1, 3, 3});
    }

    TEST_CASE("saving twice produces the same bytes") {
        const Scene scene = MakeScene();
        auto first = SceneSerializer::ToJson(scene);
        auto second = SceneSerializer::ToJson(scene);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(*first == *second);
        CHECK(first->find("\"format\"") != std::string::npos);
        CHECK(first->find("\"nodes\"") != std::string::npos);
    }

    TEST_CASE("a child with a lower entity index than its parent round trips") {
        Scene scene;
        const ecs::Entity doomed = Node(scene, "Doomed");
        const ecs::Entity parent = Node(scene, "Parent");
        scene.DestroyNode(doomed);
        // Index 0 was just freed, so this child gets a lower index than its
        // parent - the case a nested tree format cannot represent.
        const ecs::Entity child = Node(scene, "Child", parent);
        REQUIRE(child.index < parent.index);

        auto text = SceneSerializer::ToJson(scene);
        REQUIRE(text.HasValue());

        Scene loaded;
        REQUIRE(SceneSerializer::FromJson(*text, loaded).HasValue());
        REQUIRE(loaded.NodeCount() == 2);
        CHECK(loaded.Parent(FindByName(loaded, "Child")) == FindByName(loaded, "Parent"));
    }

    TEST_CASE("malformed input is an error, not a partially loaded scene") {
        Scene scene;
        CHECK(SceneSerializer::FromJson("{ not json", scene).Error().Code() ==
              StatusCode::ParseError);
        CHECK(SceneSerializer::FromJson("[1, 2, 3]", scene).Error().Code() ==
              StatusCode::ParseError);
        CHECK(SceneSerializer::FromJson("{\"format\": 1}", scene).Error().Code() ==
              StatusCode::ParseError);
    }

    TEST_CASE("a file from a newer engine is refused") {
        Scene scene;
        const auto result =
            SceneSerializer::FromJson("{\"format\": 99, \"name\": \"x\", \"nodes\": []}", scene);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::Unsupported);
    }

    TEST_CASE("a bad parent index is reported") {
        Scene scene;
        const auto result = SceneSerializer::FromJson(
            "{\"format\": 1, \"nodes\": [{\"name\": \"a\", \"parent\": 7}]}", scene);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
        CHECK(result.Error().Message().find("parent index") != std::string_view::npos);
    }

    TEST_CASE("a cycle in the file is refused") {
        Scene scene;
        const auto result = SceneSerializer::FromJson("{\"format\": 1, \"nodes\": ["
                                                     "{\"name\": \"a\", \"parent\": 1},"
                                                     "{\"name\": \"b\", \"parent\": 0}]}",
                                                     scene);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("a bad active camera index is reported") {
        Scene scene;
        const auto result = SceneSerializer::FromJson(
            "{\"format\": 1, \"nodes\": [{\"name\": \"a\"}], \"activeCamera\": 5}", scene);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("a non positive lod threshold is refused") {
        Scene scene;
        const auto result = SceneSerializer::FromJson(
            "{\"format\": 1, \"nodes\": [{\"name\": \"a\", \"meshRenderer\": {"
            "\"lods\": [\"00000000-0000-0000-0000-000000000001\"], "
            "\"lodSwitchCoverage\": [0, 0.1, 0.04]}}]}",
            scene);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("an empty scene round trips") {
        const Scene empty("Empty");
        auto text = SceneSerializer::ToJson(empty);
        REQUIRE(text.HasValue());

        Scene loaded("Placeholder");
        REQUIRE(SceneSerializer::FromJson(*text, loaded).HasValue());
        CHECK(loaded.NodeCount() == 0);
        CHECK(loaded.Name() == "Empty");
    }

    TEST_CASE("a non unit quaternion in a file is normalised on load") {
        Scene scene;
        REQUIRE(SceneSerializer::FromJson("{\"format\": 1, \"nodes\": [{\"name\": \"a\", "
                                         "\"rotation\": [0, 0, 0, 4]}]}",
                                         scene)
                    .HasValue());
        const Transform* local = scene.LocalTransform(scene.Nodes()[0]);
        REQUIRE(local != nullptr);
        CHECK(local->rotation.w == doctest::Approx(1.0f).epsilon(1e-6f));
    }
}
