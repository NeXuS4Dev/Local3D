// Renderer tests.  Culling, lod selection and cascaded shadow math are verified
// numerically; the frame graph structure and recorded work are verified against
// the null RHI.
#include "doctest.h"

#include "local3d/renderer/FrameView.hpp"
#include "local3d/renderer/Renderer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::math;
using namespace l3d::render;

namespace {

[[nodiscard]] FrameView MakeView(Vec3 position, Vec3 target, f32 fovYDegrees = 60.0f,
                                 f32 aspect = 1.0f, f32 nearPlane = 0.1f,
                                 f32 farPlane = 100.0f) {
    FrameView view;
    view.position = position;
    view.target = target;
    view.fovYDegrees = fovYDegrees;
    view.aspect = aspect;
    view.nearPlane = nearPlane;
    view.farPlane = farPlane;
    view.viewportWidth = 800;
    view.viewportHeight = 600;
    view.Update();
    return view;
}

/// A cube of `halfExtent` centred at `center`, as a draw item with one lod.
[[nodiscard]] DrawItem MakeItem(Vec3 center, f32 halfExtent, MeshHandle mesh = 0) {
    DrawItem item;
    item.lods[0] = mesh;
    item.localBounds.min = Vec3{-halfExtent, -halfExtent, -halfExtent};
    item.localBounds.max = Vec3{halfExtent, halfExtent, halfExtent};
    item.world = Mat4::Translation(center);
    return item;
}

} // namespace

TEST_SUITE("renderer.view") {
    TEST_CASE("the frustum rejects what is behind and beside the camera") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        CHECK(view.frustum.ContainsPoint(Vec3{0, 0, -10}));
        CHECK_FALSE(view.frustum.ContainsPoint(Vec3{0, 0, 10}));   // behind
        CHECK_FALSE(view.frustum.ContainsPoint(Vec3{0, 0, -200})); // past far
        CHECK_FALSE(view.frustum.ContainsPoint(Vec3{1000, 0, -10}));
    }

    TEST_CASE("screen coverage follows the projection geometry") {
        // fovY 90 with a square aspect means tan(fov/2) == 1, so the viewport is
        // 2 * distance world units tall and coverage reduces to radius/distance.
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 90.0f, 1.0f);
        CHECK(view.ScreenCoverage(1.0f, 10.0f) == doctest::Approx(0.1f));
        CHECK(view.ScreenCoverage(5.0f, 10.0f) == doctest::Approx(0.5f));
        CHECK(view.ScreenCoverage(1.0f, 0.0f) == doctest::Approx(1.0f));
    }

    TEST_CASE("viewport describes the render target") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        const auto viewport = view.Viewport();
        CHECK(viewport.width == doctest::Approx(800.0f));
        CHECK(viewport.height == doctest::Approx(600.0f));
    }

    TEST_CASE("sub-frustum corners match the projection") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 90.0f, 1.0f);
        const auto corners = ViewSpaceFrustumCorners(view, 1.0f, 4.0f);
        // Near slice at z = -1 spans [-1, 1] on both axes.
        for (usize i = 0; i < 4; ++i) {
            CHECK(corners[i].z == doctest::Approx(-1.0f));
            CHECK(std::abs(corners[i].x) == doctest::Approx(1.0f));
            CHECK(std::abs(corners[i].y) == doctest::Approx(1.0f));
        }
        // Far slice at z = -4 spans [-4, 4].
        for (usize i = 4; i < 8; ++i) {
            CHECK(corners[i].z == doctest::Approx(-4.0f));
            CHECK(std::abs(corners[i].x) == doctest::Approx(4.0f));
            CHECK(std::abs(corners[i].y) == doctest::Approx(4.0f));
        }
    }
}

TEST_SUITE("renderer.culling") {
    TEST_CASE("keeps what is in view and preserves input order") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -20}, 1.0f, 7));
        items.push_back(MakeItem(Vec3{0, 0, 20}, 1.0f, 8));  // behind the camera
        items.push_back(MakeItem(Vec3{0, 0, -40}, 1.0f, 9));
        items.push_back(MakeItem(Vec3{500, 0, -20}, 1.0f));  // far off to the side

        CullResult result;
        CullDrawItems(view, items, result);
        CHECK(result.tested == 4);
        CHECK(result.frustumCulled == 2);
        REQUIRE(result.visible.size() == 2);
        CHECK(result.visible[0] == 0);
        CHECK(result.visible[1] == 2);
        REQUIRE(result.lodIndex.size() == 2);
    }

    TEST_CASE("an empty input produces an empty result") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        CullResult result;
        CullDrawItems(view, {}, result);
        CHECK(result.tested == 0);
        CHECK(result.visible.empty());
        CHECK(result.frustumCulled == 0);
    }

    TEST_CASE("bounds follow the world transform") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        DrawItem item = MakeItem(Vec3{0, 0, 0}, 1.0f);
        item.world = Mat4::Translation(Vec3{0, 0, -20});

        std::vector<DrawItem> items{item};
        CullResult result;
        CullDrawItems(view, items, result);
        CHECK(result.visible.size() == 1);

        item.world = Mat4::Translation(Vec3{0, 0, 20});
        items[0] = item;
        CullResult culled;
        CullDrawItems(view, items, culled);
        CHECK(culled.visible.empty());
        CHECK(culled.frustumCulled == 1);
    }

    TEST_CASE("lod selection follows screen coverage") {
        DrawItem item;
        item.lods = {0, 1, 2, 3};
        item.lodSwitchCoverage = {0.35f, 0.12f, 0.04f};
        CHECK(SelectLod(item, 1.0f) == 0);
        CHECK(SelectLod(item, 0.35f) == 0);
        CHECK(SelectLod(item, 0.2f) == 1);
        CHECK(SelectLod(item, 0.05f) == 2);
        CHECK(SelectLod(item, 0.01f) == 3);

        // Only the first two lods exist: a distant item falls back to lod 1.
        DrawItem partial;
        partial.lods[0] = 0;
        partial.lods[1] = 1;
        CHECK(SelectLod(partial, 0.001f) == 1);

        // A mid-distance item wants lod 1, which does not exist here, so it must
        // fall back to a finer lod rather than selecting an empty slot.
        DrawItem single;
        single.lods[0] = 5;
        CHECK(SelectLod(single, 0.2f) == 0);
        CHECK(SelectLod(single, 1.0f) == 0);

        // With only a coarse lod available, everything falls back to it.
        DrawItem coarseOnly;
        coarseOnly.lods[2] = 9;
        CHECK(SelectLod(coarseOnly, 1.0f) == 2);
    }

    TEST_CASE("near items get the detailed lod and distant ones the coarse lod") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 60.0f, 1.0f);
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -5}, 1.0f, 0));   // near: large on screen
        items.push_back(MakeItem(Vec3{0, 0, -80}, 1.0f, 0));  // far: tiny on screen
        for (DrawItem& item : items) {
            item.lods = {0, 1, 2, 3}; // A full chain, so a coarser lod exists.
        }

        CullResult result;
        CullDrawItems(view, items, result);
        REQUIRE(result.visible.size() == 2);
        CHECK(result.lodIndex[0] == 0);
        CHECK(result.lodIndex[1] > result.lodIndex[0]);
        CHECK(result.lodHistogram[0] == 1);
        CHECK(result.lodHistogram[result.lodIndex[1]] == 1);
    }
}

TEST_SUITE("renderer.shadow_cascades") {
    TEST_CASE("splits ascend and end at the shadow distance") {
        ShadowSettings settings;
        settings.cascadeCount = 4;
        settings.maxDistance = 100.0f;
        settings.lambda = 0.6f;
        const auto splits = ComputeCascadeSplits(0.1f, 500.0f, settings);
        REQUIRE(splits.size() == 4);
        for (usize i = 1; i < splits.size(); ++i) {
            CHECK(splits[i] > splits[i - 1]);
        }
        CHECK(splits.back() == doctest::Approx(100.0f));
    }

    TEST_CASE("lambda interpolates between linear and logarithmic") {
        ShadowSettings linear;
        linear.cascadeCount = 4;
        linear.maxDistance = 100.0f;
        linear.lambda = 0.0f;
        const auto linearSplits = ComputeCascadeSplits(1.0f, 500.0f, linear);
        REQUIRE(linearSplits.size() == 4);
        // Pure linear: 1, 25.75, 50.5, 75.25, 100.
        CHECK(linearSplits[0] == doctest::Approx(25.75f));
        CHECK(linearSplits[1] == doctest::Approx(50.5f));

        ShadowSettings logarithmic = linear;
        logarithmic.lambda = 1.0f;
        const auto logSplits = ComputeCascadeSplits(1.0f, 500.0f, logarithmic);
        // Logarithmic puts the first split much closer to the camera.
        CHECK(logSplits[0] < linearSplits[0]);
        CHECK(logSplits.back() == doctest::Approx(100.0f));
    }

    TEST_CASE("cascade count is clamped to the supported maximum") {
        ShadowSettings settings;
        settings.cascadeCount = 16;
        CHECK(settings.ClampedCascadeCount() == kMaxShadowCascades);
        const auto splits = ComputeCascadeSplits(0.1f, 100.0f, settings);
        CHECK(splits.size() == kMaxShadowCascades);

        ShadowSettings zero;
        zero.cascadeCount = 0;
        CHECK(zero.ClampedCascadeCount() == 1);
        CHECK(ComputeCascadeSplits(0.1f, 100.0f, zero).size() == 1);
    }

    TEST_CASE("the shadow distance is clamped into the camera range") {
        ShadowSettings settings;
        settings.cascadeCount = 2;
        settings.maxDistance = 1000.0f; // Beyond the far plane.
        const auto splits = ComputeCascadeSplits(1.0f, 50.0f, settings);
        REQUIRE(splits.size() == 2);
        CHECK(splits.back() == doctest::Approx(50.0f));
    }

    TEST_CASE("every slice corner lands inside the cascade's clip space") {
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        ShadowSettings settings;
        settings.cascadeCount = 3;
        settings.mapSize = 1024;
        settings.maxDistance = 60.0f;
        const auto splits = ComputeCascadeSplits(view.nearPlane, view.farPlane, settings);

        f32 previous = view.nearPlane;
        for (const f32 split : splits) {
            const ShadowCascade cascade =
                ComputeCascade(view, Vec3{-0.3f, -1.0f, -0.2f}, previous, split, settings);
            const auto corners = ViewSpaceFrustumCorners(view, previous, split);
            const Mat4 inverseView = view.view.Inverse();
            for (const Vec3& corner : corners) {
                const Vec3 clip = cascade.viewProjection.TransformPoint(
                    inverseView.TransformPoint(corner));
                // Vulkan clip space: x, y in [-1, 1] and z in [0, 1].
                CHECK(std::abs(clip.x) <= 1.0f + 1e-3f);
                CHECK(std::abs(clip.y) <= 1.0f + 1e-3f);
                CHECK(clip.z >= -1e-3f);
                CHECK(clip.z <= 1.0f + 1e-3f);
            }
            CHECK(cascade.splitViewDepth == doctest::Approx(split));
            CHECK(cascade.texelWorldSize > 0.0f);
            previous = split;
        }
    }

    TEST_CASE("a light parallel to the up vector still produces a finite matrix") {
        // A sun pointing straight down is the common case and leaves LookAt with
        // a degenerate basis unless the up vector is switched.
        const FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        ShadowSettings settings;
        settings.cascadeCount = 2;
        settings.mapSize = 1024;
        const auto splits = ComputeCascadeSplits(view.nearPlane, view.farPlane, settings);
        REQUIRE(splits.size() == 2);

        const math::Vec3 directions[] = {Vec3{0, -1, 0}, Vec3{0, 1, 0}, Vec3{0, -0.9999f, 0}};
        for (const math::Vec3& direction : directions) {
            const ShadowCascade cascade =
                ComputeCascade(view, direction, view.nearPlane, splits[0], settings);
            for (u32 i = 0; i < 16; ++i) {
                const f32 value = cascade.viewProjection.Data()[i];
                CHECK(std::isfinite(value));
            }
            CHECK(cascade.texelWorldSize > 0.0f);
        }
    }

    TEST_CASE("texel snapping keeps the shadow map stable as the camera moves") {
        ShadowSettings settings;
        settings.cascadeCount = 1;
        settings.mapSize = 512;
        settings.maxDistance = 40.0f;
        settings.texelSnapping = true;

        const Vec3 lightDirection{-0.2f, -1.0f, 0.0f};
        const FrameView first = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        const auto firstSplits = ComputeCascadeSplits(first.nearPlane, first.farPlane, settings);
        const ShadowCascade snapped =
            ComputeCascade(first, lightDirection, first.nearPlane, firstSplits[0], settings);

        // A sub-texel camera shift.  Without snapping the ortho box origin moves
        // continuously and shadows shimmer.
        const FrameView shifted = MakeView(Vec3{0.02f, 0, 0}, Vec3{0.02f, 0, -1});
        const auto shiftedSplits =
            ComputeCascadeSplits(shifted.nearPlane, shifted.farPlane, settings);
        const ShadowCascade snappedShifted =
            ComputeCascade(shifted, lightDirection, shifted.nearPlane, shiftedSplits[0], settings);

        // The same world point must project to (almost) the same shadow texel.
        const Vec3 worldPoint{3.0f, 1.0f, -12.0f};
        const Vec3 a = snapped.viewProjection.TransformPoint(worldPoint);
        const Vec3 b = snappedShifted.viewProjection.TransformPoint(worldPoint);
        const f32 texelNdc = 2.0f / static_cast<f32>(settings.mapSize);
        CHECK(std::abs(a.x - b.x) < texelNdc);
        CHECK(std::abs(a.y - b.y) < texelNdc);
    }
}

namespace {

[[nodiscard]] std::vector<u8> FakeShader(usize size = 64) { return std::vector<u8>(size, 0x42); }

/// A complete library, since the renderer refuses to run with a missing pass.
[[nodiscard]] ShaderLibrary MakeShaderLibrary() {
    ShaderLibrary library;
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

[[nodiscard]] MeshData MakeCube(const std::string& name = "cube") {
    MeshData mesh;
    mesh.name = name;
    // Two triangles are enough to exercise the upload path.
    mesh.positions = {Vec3{-1, -1, 0}, Vec3{1, -1, 0}, Vec3{1, 1, 0}, Vec3{-1, 1, 0}};
    mesh.normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    mesh.uvs = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.bounds.min = Vec3{-1, -1, 0};
    mesh.bounds.max = Vec3{1, 1, 0};
    return mesh;
}

/// Device plus an initialised renderer with two registered meshes.
class RendererFixture {
public:
    explicit RendererFixture(RendererSettings settings = {}) : settings_(std::move(settings)) {
        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        desc.enableValidation = true;
        auto device = rhi::CreateDevice(desc);
        REQUIRE_MESSAGE(device.HasValue(), "Failed to create the null device");
        device_ = std::move(*device);

        renderer_ = std::make_unique<Renderer>(settings_);
        auto initialized = renderer_->Initialize(*device_, MakeShaderLibrary());
        REQUIRE_MESSAGE(initialized.HasValue(), "Renderer failed to initialise");

        auto first = renderer_->RegisterMesh(*device_, MakeCube("cube-a"));
        REQUIRE(first.HasValue());
        meshA_ = *first;
        auto second = renderer_->RegisterMesh(*device_, MakeCube("cube-b"));
        REQUIRE(second.HasValue());
        meshB_ = *second;

        auto commands = device_->CreateCommandBuffer();
        REQUIRE(commands.HasValue());
        commands_ = std::move(*commands);
    }

    [[nodiscard]] rhi::IDevice& Device() { return *device_; }
    [[nodiscard]] Renderer& Render() { return *renderer_; }
    [[nodiscard]] rhi::ICommandBuffer& Commands() { return *commands_; }
    [[nodiscard]] MeshHandle MeshA() const { return meshA_; }
    [[nodiscard]] MeshHandle MeshB() const { return meshB_; }
    [[nodiscard]] usize RhiErrors() const { return device_->ValidationErrorCount(); }
    [[nodiscard]] std::vector<std::string> Errors() const { return device_->ValidationErrors(); }

    /// Runs one frame over `items` and returns the result.
    [[nodiscard]] Result<void> Frame(std::span<const DrawItem> items,
                                     DirectionalLight sun = {}) {
        FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1}, 60.0f,
                                  static_cast<f32>(settings_.width) /
                                      static_cast<f32>(settings_.height));
        Commands().Begin();
        auto result = renderer_->RenderFrame(*device_, Commands(), view, items, sun, {});
        Commands().End();
        const auto& stats = renderer_->Stats();
        MESSAGE("stats: items=", stats.drawItems, " visible=", stats.visibleItems,
                " culled=", stats.frustumCulled, " batches=", stats.batches,
                " instances=", stats.instances, " draws=", stats.drawCalls,
                " dispatches=", stats.dispatches, " passes=", stats.graphPasses,
                " culledPasses=", stats.graphCulledPasses, " transientBytes=",
                stats.transientBytes);
        for (const std::string& error : device_->ValidationErrors()) {
            MESSAGE("  rhi: ", error);
        }
        if (result.IsError()) {
            MESSAGE("RenderFrame error: ", result.Error().Message());
            for (const std::string& error : renderer_->Graph().ValidationErrors()) {
                MESSAGE("  graph: ", error);
            }
        }
        return result;
    }

private:
    RendererSettings settings_;
    std::unique_ptr<rhi::IDevice> device_;
    std::unique_ptr<Renderer> renderer_;
    rhi::CommandBufferPtr commands_;
    MeshHandle meshA_ = kInvalidMesh;
    MeshHandle meshB_ = kInvalidMesh;
};

} // namespace

TEST_SUITE("renderer.initialisation") {
    TEST_CASE("refuses to run with a missing shader") {
        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());

        Renderer renderer(RendererSettings{});
        ShaderLibrary partial = MakeShaderLibrary();
        partial.bloomDownsample.clear();
        auto result = renderer.Initialize(**device, partial);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
        CHECK(std::string(result.Error().Message()).find("bloom") != std::string::npos);
        CHECK_FALSE(renderer.IsInitialized());

        // A renderer that does not need bloom does not need its shaders.
        RendererSettings noBloom;
        noBloom.enableBloom = false;
        Renderer lean(noBloom);
        CHECK(lean.Initialize(**device, partial).HasValue());
    }

    TEST_CASE("rejects a zero sized target") {
        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());
        RendererSettings settings;
        settings.width = 0;
        Renderer renderer(settings);
        CHECK(renderer.Initialize(**device, MakeShaderLibrary()).IsError());
    }

    TEST_CASE("rendering before initialisation is an error") {
        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());
        auto commands = (*device)->CreateCommandBuffer();
        REQUIRE(commands.HasValue());
        Renderer renderer(RendererSettings{});
        FrameView view = MakeView(Vec3{0, 0, 0}, Vec3{0, 0, -1});
        auto result = renderer.RenderFrame(**device, **commands, view, {}, {}, {});
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidState);
    }

    TEST_CASE("validates mesh data before uploading") {
        rhi::DeviceDesc desc;
        desc.preferredBackend = rhi::BackendType::Null;
        auto device = rhi::CreateDevice(desc);
        REQUIRE(device.HasValue());
        Renderer renderer(RendererSettings{});
        REQUIRE(renderer.Initialize(**device, MakeShaderLibrary()).HasValue());

        MeshData incomplete;
        incomplete.name = "broken";
        incomplete.positions = {Vec3{0, 0, 0}};
        CHECK(renderer.RegisterMesh(**device, incomplete).IsError());

        auto valid = renderer.RegisterMesh(**device, MakeCube());
        REQUIRE(valid.HasValue());
        CHECK(renderer.MeshCount() == 1);
    }
}

TEST_SUITE("renderer.frame") {
    TEST_CASE("renders a full frame and records the expected work") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        DrawItem item = MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA());
        items.push_back(item);

        auto result = fixture.Frame(items);
        REQUIRE_MESSAGE(result.HasValue(), "RenderFrame failed");

        const auto& stats = fixture.Render().Stats();
        CHECK(stats.drawItems == 1);
        CHECK(stats.visibleItems == 1);
        CHECK(stats.frustumCulled == 0);
        CHECK(stats.batches == 1);
        CHECK(stats.instances == 1);
        CHECK(stats.shadowCasters == 1);
        CHECK(stats.graphPasses > 0);
        CHECK(stats.graphCulledPasses == 0);
        CHECK(stats.transientBytes > 0);
        // 15 passes: shadow, prepass, forward, ssao, 10 bloom, tonemap.
        CHECK(stats.graphPasses == 15);
        // 4 cascades + prepass + forward + tonemap.
        CHECK(stats.drawCalls == 7);
        // ssao + 10 bloom passes.
        CHECK(stats.dispatches == 11);
        CHECK(fixture.RhiErrors() == 0);
        CHECK(fixture.Render().Graph().ValidationErrorCount() == 0);
    }

    TEST_CASE("items behind the camera are culled and draw nothing") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, 10}, 1.0f, fixture.MeshA()));

        auto result = fixture.Frame(items);
        REQUIRE(result.HasValue());
        const auto& stats = fixture.Render().Stats();
        CHECK(stats.visibleItems == 0);
        CHECK(stats.frustumCulled == 1);
        CHECK(stats.batches == 0);
        // No geometry draws at all: the shadow and prepass passes skip when there
        // are no batches.  Only the tonemap still runs over the cleared frame.
        CHECK(stats.drawCalls == 1);
        CHECK(stats.shadowCasters == 0);
        CHECK(fixture.RhiErrors() == 0);
    }

    TEST_CASE("identical meshes are instanced into one draw call") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        for (int i = 0; i < 3; ++i) {
            items.push_back(MakeItem(Vec3{static_cast<f32>(i), 0, -10}, 1.0f, fixture.MeshA()));
        }
        auto result = fixture.Frame(items);
        REQUIRE(result.HasValue());
        const auto& stats = fixture.Render().Stats();
        CHECK(stats.instances == 3);
        CHECK(stats.batches == 1);
        // One instanced draw per pass that draws geometry: 4 cascades, prepass,
        // forward; the tonemap is a single fullscreen triangle.
        CHECK(stats.drawCalls == 7);
        REQUIRE(fixture.Render().LastInstances().size() == 3);
        CHECK(fixture.Render().LastInstances()[1].tint.x == doctest::Approx(1.0f));
    }

    TEST_CASE("different meshes are batched separately") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA()));
        items.push_back(MakeItem(Vec3{2, 0, -10}, 1.0f, fixture.MeshB()));
        items.push_back(MakeItem(Vec3{4, 0, -10}, 1.0f, fixture.MeshA()));

        auto result = fixture.Frame(items);
        REQUIRE(result.HasValue());
        // Grouping is by consecutive run, so A, B, A is three batches.
        CHECK(fixture.Render().Stats().batches == 3);
        CHECK(fixture.Render().Stats().instances == 3);
    }

    TEST_CASE("non shadow casters stay out of the shadow pass") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        DrawItem caster = MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA());
        DrawItem receiver = MakeItem(Vec3{3, 0, -10}, 1.0f, fixture.MeshA());
        receiver.castsShadow = false;
        items.push_back(caster);
        items.push_back(receiver);

        auto result = fixture.Frame(items);
        REQUIRE(result.HasValue());
        CHECK(fixture.Render().Stats().instances == 2);
        CHECK(fixture.Render().Stats().shadowCasters == 1);
    }

    TEST_CASE("disabling features removes their passes") {
        RendererSettings settings;
        settings.enableBloom = false;
        settings.enableSsao = false;
        settings.enableShadows = false;
        RendererFixture fixture(settings);
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA()));

        auto result = fixture.Frame(items);
        REQUIRE(result.HasValue());
        const auto& stats = fixture.Render().Stats();
        // Only the forward pass and the tonemap remain.
        CHECK(stats.graphPasses == 2);
        CHECK(stats.dispatches == 0);
        CHECK(stats.drawCalls == 2);
        CHECK(stats.shadowCasters == 0);
        CHECK(fixture.RhiErrors() == 0);
    }

    TEST_CASE("cascade count follows the settings") {
        RendererSettings settings;
        settings.shadows.cascadeCount = 2;
        settings.shadows.mapSize = 512;
        RendererFixture fixture(settings);
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA()));

        auto result = fixture.Frame(items);
        REQUIRE(result.HasValue());
        REQUIRE(fixture.Render().Cascades().size() == 2);
        CHECK(fixture.Render().Uniforms().cascadeSplits.y > 0.0f);
        CHECK(fixture.Render().Uniforms().cascadeSplits.z == doctest::Approx(0.0f));
        // flags packs the cascade count with the feature bits: 2 + ssao 16 +
        // bloom 32 + taa 64.
        CHECK(fixture.Render().Uniforms().flagsAndParams.x == doctest::Approx(2.0f + 16 + 32 + 64));
        // Two cascades + prepass + forward + tonemap.
        CHECK(fixture.Render().Stats().drawCalls == 5);
    }

    TEST_CASE("the uploaded uniform block matches the CPU copy") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA()));
        REQUIRE(fixture.Frame(items).HasValue());

        void* mapped = fixture.Render().UniformBuffer().Map();
        REQUIRE(mapped != nullptr);
        FrameUniforms uploaded;
        std::memcpy(&uploaded, mapped, sizeof(FrameUniforms));
        fixture.Render().UniformBuffer().Unmap();

        const FrameUniforms& expected = fixture.Render().Uniforms();
        CHECK(uploaded.cameraPositionAndExposure.w == doctest::Approx(expected.cameraPositionAndExposure.w));
        CHECK(uploaded.viewProjection.Data()[0] == doctest::Approx(expected.viewProjection.Data()[0]));
        CHECK(uploaded.flagsAndParams.x == doctest::Approx(expected.flagsAndParams.x));
        for (u32 i = 0; i < 16; ++i) {
            CHECK(uploaded.cascadeViewProjections[0].Data()[i] ==
                  doctest::Approx(expected.cascadeViewProjections[0].Data()[i]));
        }
        CHECK(uploaded.renderTargetSize.x == doctest::Approx(1280.0f));
    }

    TEST_CASE("the instance buffer holds what was uploaded") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        DrawItem item = MakeItem(Vec3{5, 6, -10}, 1.0f, fixture.MeshA());
        item.tint = Vec4{0.25f, 0.5f, 0.75f, 1.0f};
        items.push_back(item);
        REQUIRE(fixture.Frame(items).HasValue());

        const rhi::IBuffer* instances = fixture.Render().InstanceBuffer();
        REQUIRE(instances != nullptr);
        CHECK(instances->Size() >= sizeof(GpuInstanceData));
        void* mapped = const_cast<rhi::IBuffer*>(instances)->Map();
        REQUIRE(mapped != nullptr);
        GpuInstanceData uploaded;
        std::memcpy(&uploaded, mapped, sizeof(GpuInstanceData));
        const_cast<rhi::IBuffer*>(instances)->Unmap();
        CHECK(uploaded.tint.x == doctest::Approx(0.25f));
        CHECK(uploaded.world.Data()[12] == doctest::Approx(5.0f));
        CHECK(uploaded.world.Data()[13] == doctest::Approx(6.0f));
    }

    TEST_CASE("a second frame reuses the frame resources") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA()));

        REQUIRE(fixture.Frame(items).HasValue());
        const u32 afterFirst = fixture.Device().MemoryUsage().textureCount;
        fixture.Device().BeginFrame();
        fixture.Device().EndFrame();
        REQUIRE(fixture.Frame(items).HasValue());
        CHECK(fixture.Device().MemoryUsage().textureCount == afterFirst);
        CHECK(fixture.RhiErrors() == 0);
    }

    TEST_CASE("resizing rebuilds the frame targets") {
        RendererFixture fixture;
        std::vector<DrawItem> items;
        items.push_back(MakeItem(Vec3{0, 0, -10}, 1.0f, fixture.MeshA()));
        REQUIRE(fixture.Frame(items).HasValue());
        const u64 firstBytes = fixture.Render().Stats().transientBytes;

        fixture.Render().Resize(640, 360);
        REQUIRE(fixture.Frame(items).HasValue());
        const u64 smallerBytes = fixture.Render().Stats().transientBytes;
        CHECK(smallerBytes < firstBytes);
        CHECK(fixture.Render().Uniforms().renderTargetSize.x == doctest::Approx(640.0f));
        CHECK(fixture.RhiErrors() == 0);

        // Resizing to the same size changes nothing.
        fixture.Render().Resize(640, 360);
        REQUIRE(fixture.Frame(items).HasValue());
        CHECK(fixture.Render().Stats().transientBytes == smallerBytes);
    }

    TEST_CASE("an empty scene still produces a valid frame") {
        RendererFixture fixture;
        auto result = fixture.Frame({});
        REQUIRE(result.HasValue());
        CHECK(fixture.Render().Stats().drawItems == 0);
        // The frame is still presented, so the tonemap pass draws.
        CHECK(fixture.Render().Stats().drawCalls == 1);
        CHECK(fixture.RhiErrors() == 0);
    }
}
