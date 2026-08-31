// Render graph tests.  The graph's contract is ordering, culling and lifetime
// bookkeeping; these tests also execute a compiled graph against the null RHI so
// the transient allocation path is covered end to end.
#include "doctest.h"

#include "local3d/rendergraph/RenderGraph.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::graph;
using namespace l3d::rhi;

namespace {

constexpr u32 kWidth = 512;
constexpr u32 kHeight = 512;

[[nodiscard]] std::unique_ptr<IDevice> MakeDevice() {
    DeviceDesc desc;
    desc.preferredBackend = BackendType::Null;
    desc.enableValidation = true;
    auto result = CreateDevice(desc);
    REQUIRE_MESSAGE(result.HasValue(), "Failed to create the null device");
    return std::move(*result);
}

[[nodiscard]] ConstByteSpan AsBytesOf(const std::vector<u8>& data) {
    return std::as_bytes(std::span(data.data(), data.size()));
}

[[nodiscard]] TextureSpec ColorTargetSpec(std::string name) {
    TextureSpec spec;
    spec.name = std::move(name);
    spec.width = kWidth;
    spec.height = kHeight;
    spec.format = Format::RGBA16_Float;
    spec.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
    return spec;
}

[[nodiscard]] TextureSpec DepthTargetSpec(std::string name) {
    TextureSpec spec;
    spec.name = std::move(name);
    spec.width = kWidth;
    spec.height = kHeight;
    spec.format = Format::Depth32_Float;
    spec.usage = TextureUsage::DepthStencilAttachment;
    return spec;
}

[[nodiscard]] BufferSpec StorageBufferSpec(std::string name, u64 size = 4096) {
    BufferSpec spec;
    spec.name = std::move(name);
    spec.size = size;
    spec.usage = BufferUsage::Storage | BufferUsage::CopyDestination;
    return spec;
}

/// A null device plus the render state a raster pass needs.  Creating these once
/// per test case mirrors how a renderer caches pipelines across frames.
class GraphFixture {
public:
    GraphFixture() {
        device_ = MakeDevice();
        auto commands = device_->CreateCommandBuffer();
        REQUIRE(commands.HasValue());
        commands_ = std::move(*commands);

        const std::vector<u8> vertexCode(64, 0x42);
        const std::vector<u8> fragmentCode(64, 0x42);
        const std::vector<u8> computeCode(64, 0x42);

        ShaderModuleDesc vertexStage;
        vertexStage.stage = ShaderStage::Vertex;
        vertexStage.bytecode = AsBytesOf(vertexCode);
        vertexStage.debugName = "vs";
        ShaderModuleDesc fragmentStage;
        fragmentStage.stage = ShaderStage::Fragment;
        fragmentStage.bytecode = AsBytesOf(fragmentCode);
        fragmentStage.debugName = "fs";

        PipelineDesc graphicsDesc;
        graphicsDesc.shaders.PushBack(vertexStage);
        graphicsDesc.shaders.PushBack(fragmentStage);
        graphicsDesc.colorFormats.PushBack(Format::RGBA16_Float);
        graphicsDesc.depthFormat = Format::Depth32_Float;
        graphicsDesc.hasDepthAttachment = true;
        graphicsDesc.debugName = "graph-test";
        auto graphics = device_->CreateGraphicsPipeline(graphicsDesc);
        REQUIRE(graphics.HasValue());
        graphics_ = std::move(*graphics);

        PipelineDesc computeDesc;
        ShaderModuleDesc computeStage;
        computeStage.stage = ShaderStage::Compute;
        computeStage.bytecode = AsBytesOf(computeCode);
        computeStage.debugName = "cs";
        computeDesc.shaders.PushBack(computeStage);
        auto compute = device_->CreateComputePipeline(computeDesc);
        REQUIRE(compute.HasValue());
        compute_ = std::move(*compute);

        RenderPassDesc passDesc;
        AttachmentDesc colorAttachment;
        colorAttachment.format = Format::RGBA16_Float;
        passDesc.colorAttachments.PushBack(colorAttachment);
        passDesc.hasDepthStencil = true;
        passDesc.depthStencil.format = Format::Depth32_Float;
        auto pass = device_->CreateRenderPass(passDesc);
        REQUIRE(pass.HasValue());
        renderPass_ = std::move(*pass);
    }

    [[nodiscard]] IDevice& Device() { return *device_; }
    [[nodiscard]] ICommandBuffer& Commands() { return *commands_; }
    [[nodiscard]] IPipeline& GraphicsPipeline() { return *graphics_; }
    [[nodiscard]] IPipeline& ComputePipeline() { return *compute_; }
    [[nodiscard]] IRenderPass& RenderPass() { return *renderPass_; }
    [[nodiscard]] usize RhiErrors() const { return device_->ValidationErrorCount(); }

    /// A framebuffer per attachment set, recreated only when the graph hands out
    /// different transients - which is exactly what happens on a resize.
    struct FramebufferCache {
        FramebufferPtr framebuffer;
        const ITexture* color = nullptr;
        const ITexture* depth = nullptr;
    };

    /// Records a full screen triangle into the pass's declared targets.
    void RecordDraw(PassContext& context, TextureHandle color, TextureHandle depth,
                    FramebufferCache& cache) {
        auto* colorTexture = context.Texture(color);
        auto* depthTexture = context.Texture(depth);
        REQUIRE(colorTexture != nullptr);
        REQUIRE(depthTexture != nullptr);
        if (cache.color != colorTexture || cache.depth != depthTexture) {
            std::array<const ITexture* const, 2> attachments{colorTexture, depthTexture};
            auto framebuffer = context.Device().CreateFramebuffer(RenderPass(), attachments,
                                                                  kWidth, kHeight, "graph-target");
            REQUIRE(framebuffer.HasValue());
            cache.framebuffer = std::move(*framebuffer);
            cache.color = colorTexture;
            cache.depth = depthTexture;
        }
        std::array<ClearValue, 1> clearValues{};
        context.Commands().BeginRenderPass(RenderPass(), *cache.framebuffer, clearValues);
        context.Commands().BindPipeline(GraphicsPipeline());
        Viewport viewport;
        viewport.width = static_cast<f32>(kWidth);
        viewport.height = static_cast<f32>(kHeight);
        context.Commands().SetViewport(viewport);
        context.Commands().Draw(3);
        context.Commands().EndRenderPass();
    }

private:
    std::unique_ptr<IDevice> device_;
    CommandBufferPtr commands_;
    PipelinePtr graphics_;
    PipelinePtr compute_;
    RenderPassPtr renderPass_;
};

} // namespace

TEST_SUITE("rendergraph.compile") {
    TEST_CASE("orders passes by their data dependencies") {
        RenderGraph graph;
        const auto scene = graph.CreateTexture(ColorTargetSpec("scene-color"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("scene-depth"));
        const auto resolved = graph.CreateTexture(ColorTargetSpec("resolved"));
        graph.MarkOutput(resolved);

        // Declared out of order on purpose: the graph must still run the scene
        // pass before the pass that samples its result.
        auto resolve = graph.AddRasterPass(
            "resolve",
            [&](PassBuilder& builder) {
                builder.Read(scene);
                builder.Write(resolved);
            },
            [](PassContext&) {});
        auto scenePass = graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) {
                builder.Write(scene);
                builder.Write(depth);
            },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        const auto order = graph.ExecutionOrder();
        REQUIRE(order.size() == 2);
        CHECK(order[0] == scenePass);
        CHECK(order[1] == resolve);
        CHECK(graph.Stats().executedPasses == 2);
        CHECK(graph.Stats().culledPasses == 0);
        CHECK(graph.Stats().edges >= 1);
        CHECK(graph.ValidationErrorCount() == 0);
    }

    TEST_CASE("culls passes whose output nothing consumes") {
        RenderGraph graph;
        const auto used = graph.CreateTexture(ColorTargetSpec("used"));
        const auto unused = graph.CreateTexture(ColorTargetSpec("unused"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        graph.MarkOutput(used);

        auto kept = graph.AddRasterPass(
            "kept",
            [&](PassBuilder& builder) {
                builder.Write(used);
                builder.Write(depth);
            },
            [](PassContext&) {});
        auto dead = graph.AddRasterPass(
            "dead",
            [&](PassBuilder& builder) { builder.Write(unused); },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK(graph.Stats().culledPasses == 1);
        CHECK(graph.Pass(dead).culled);
        CHECK_FALSE(graph.Pass(kept).culled);
        REQUIRE(graph.ExecutionOrder().size() == 1);
        CHECK(graph.ExecutionOrder()[0] == kept);
    }

    TEST_CASE("a culled pass does not drag its producers down with it") {
        RenderGraph graph;
        const auto intermediate = graph.CreateTexture(ColorTargetSpec("intermediate"));
        const auto finalTarget = graph.CreateTexture(ColorTargetSpec("final"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        graph.MarkOutput(finalTarget);

        auto producer = graph.AddRasterPass(
            "producer",
            [&](PassBuilder& builder) {
                builder.Write(intermediate);
                builder.Write(depth);
            },
            [](PassContext&) {});
        // Nothing reads `intermediate`, and this pass's own output is unused, so
        // both go; the graph must not keep the producer alive for a dead reader.
        auto consumer = graph.AddRasterPass(
            "consumer",
            [&](PassBuilder& builder) {
                builder.Read(intermediate);
                builder.Write(graph.CreateTexture(ColorTargetSpec("throwaway")));
            },
            [](PassContext&) {});
        auto finalPass = graph.AddRasterPass(
            "final-pass",
            [&](PassBuilder& builder) { builder.Write(finalTarget); },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK(graph.Pass(consumer).culled);
        CHECK(graph.Pass(producer).culled);
        CHECK_FALSE(graph.Pass(finalPass).culled);
        REQUIRE(graph.ExecutionOrder().size() == 1);
    }

    TEST_CASE("side effect passes are never culled") {
        RenderGraph graph;
        const auto presented = graph.CreateTexture(ColorTargetSpec("presented"));
        auto present = graph.AddRasterPass(
            "present",
            [&](PassBuilder& builder) { builder.Write(presented); },
            [](PassContext&) {});
        graph.MarkSideEffects(present);

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK_FALSE(graph.Pass(present).culled);
        CHECK(graph.Stats().culledPasses == 0);
    }

    TEST_CASE("detects a dependency cycle") {
        RenderGraph graph;
        const auto a = graph.CreateTexture(ColorTargetSpec("a"));
        const auto b = graph.CreateTexture(ColorTargetSpec("b"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        graph.MarkOutput(a);
        graph.MarkOutput(b);

        graph.AddRasterPass(
            "first",
            [&](PassBuilder& builder) {
                builder.Read(b);
                builder.Write(a);
                builder.Write(depth);
            },
            [](PassContext&) {});
        graph.AddRasterPass(
            "second",
            [&](PassBuilder& builder) {
                builder.Read(a);
                builder.Write(b);
            },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.IsError());
        CHECK(compiled.Error().Code() == StatusCode::InvalidState);
        const std::string message(compiled.Error().Message());
        CHECK(message.find("cycle") != std::string::npos);
    }

    TEST_CASE("rejects an unknown resource handle") {
        RenderGraph graph;
        RenderGraph other;
        const auto foreign = other.CreateTexture(ColorTargetSpec("foreign"));
        graph.AddRasterPass(
            "bad",
            [&](PassBuilder& builder) { builder.Write(foreign); },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.IsError());
        CHECK(compiled.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("validates pass kinds against what they write") {
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        const auto storage = graph.CreateBuffer(StorageBufferSpec("storage"));
        graph.MarkOutput(storage);

        // A raster pass must write a color or depth attachment.
        graph.AddRasterPass(
            "raster-without-target",
            [&](PassBuilder& builder) { builder.Write(storage); },
            [](PassContext&) {});
        // A compute pass must not write a render target.
        graph.AddComputePass(
            "compute-writing-target",
            [&](PassBuilder& builder) {
                builder.Read(storage);
                builder.Write(color);
            },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK(graph.ValidationErrorCount() == 2);
        const auto& errors = graph.ValidationErrors();
        REQUIRE(errors.size() == 2);
        CHECK(errors[0].find("writes no color or depth attachment") != std::string::npos);
        CHECK(errors[1].find("writes a render target") != std::string::npos);
    }

    TEST_CASE("reports resource lifetimes in execution order") {
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        const auto post = graph.CreateTexture(ColorTargetSpec("post"));
        graph.MarkOutput(post);

        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) {
                builder.Write(color);
                builder.Write(depth);
            },
            [](PassContext&) {});
        graph.AddRasterPass(
            "post",
            [&](PassBuilder& builder) {
                builder.Read(color);
                builder.Write(post);
            },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK(graph.Texture(color).firstUse == 0);
        CHECK(graph.Texture(color).lastUse == 1);
        CHECK(graph.Texture(depth).firstUse == 0);
        CHECK(graph.Texture(depth).lastUse == 0);
        CHECK(graph.Texture(post).firstUse == 1);
        CHECK(graph.Texture(color).writerCount == 1);
        CHECK(graph.Texture(color).readerCount == 1);
    }

    TEST_CASE("reports transient memory") {
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        graph.MarkOutput(color);
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) {
                builder.Write(color);
                builder.Write(depth);
            },
            [](PassContext&) {});

        // 512x512 RGBA16F = 524288 bytes, 512x512 D32F = 1048576 bytes.
        const u64 expected = 512ULL * 512 * 8 + 512ULL * 512 * 4;
        CHECK(graph.Stats().transientBytes == 0);
        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK(graph.Stats().transientBytes == expected);
        CHECK(graph.Stats().transientTextureCount == 2);
        CHECK(graph.Texture(color).sizeBytes == 512ULL * 512 * 8);
        CHECK_FALSE(graph.Texture(color).isExternal);
    }

    TEST_CASE("imported resources are external and not counted as transient") {
        GraphFixture fixture;
        auto owned = fixture.Device().CreateTexture(ToRhiDesc(ColorTargetSpec("imported")));
        REQUIRE(owned.HasValue());

        RenderGraph graph;
        const auto imported = graph.ImportTexture(**owned, "imported");
        graph.MarkOutput(imported);
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        auto pass = graph.AddRasterPass(
            "draw",
            [&](PassBuilder& builder) {
                builder.Write(imported);
                builder.Write(depth);
            },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        CHECK(graph.Texture(imported).isExternal);
        CHECK(graph.Texture(imported).name == "imported");
        CHECK(graph.Stats().transientTextureCount == 1);
        CHECK_FALSE(graph.Pass(pass).culled);
    }

    TEST_CASE("executing before compiling is an error") {
        GraphFixture fixture;
        RenderGraph graph;
        auto result = graph.Execute(fixture.Device(), fixture.Commands());
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidState);
    }
}

TEST_SUITE("rendergraph.execute") {
    TEST_CASE("executes the surviving passes and records their work") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("scene-color"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("scene-depth"));
        graph.MarkOutput(color);

        GraphFixture::FramebufferCache cache;
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) {
                builder.Write(color);
                builder.Write(depth);
            },
            [&](PassContext& context) { fixture.RecordDraw(context, color, depth, cache); });

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());

        fixture.Commands().Begin();
        auto executed = graph.Execute(fixture.Device(), fixture.Commands());
        fixture.Commands().End();
        REQUIRE(executed.HasValue());
        CHECK(graph.ValidationErrorCount() == 0);

        // The pass really recorded a draw against the null backend.
        CHECK(fixture.Commands().GetStats().drawCalls == 1);
        CHECK(fixture.Commands().GetStats().renderPasses == 1);
        CHECK(fixture.RhiErrors() == 0);

        // The graph allocated exactly the two transients it promised.
        CHECK(fixture.Device().MemoryUsage().textureCount == 2);
        CHECK(cache.framebuffer != nullptr);
    }

    TEST_CASE("reuses transient allocations across frames") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        graph.MarkOutput(color);

        GraphFixture::FramebufferCache cache;
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) {
                builder.Write(color);
                builder.Write(depth);
            },
            [&](PassContext& context) { fixture.RecordDraw(context, color, depth, cache); });

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());

        for (int frame = 0; frame < 3; ++frame) {
            fixture.Commands().Begin();
            auto executed = graph.Execute(fixture.Device(), fixture.Commands());
            fixture.Commands().End();
            REQUIRE(executed.HasValue());
            fixture.Device().BeginFrame();
            fixture.Device().EndFrame();
        }

        // Three frames, still two textures: the cache handed the same objects
        // back instead of allocating per frame.
        CHECK(fixture.Device().MemoryUsage().textureCount == 2);
        CHECK(fixture.Commands().GetStats().drawCalls == 1);
        // The framebuffer was built once and stayed valid for all three frames.
        CHECK(cache.color != nullptr);
    }

    TEST_CASE("release drops every cached transient") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        graph.MarkOutput(color);
        graph.AddRasterPass(
            "clear",
            [&](PassBuilder& builder) { builder.Write(color); },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        fixture.Commands().Begin();
        REQUIRE(graph.Execute(fixture.Device(), fixture.Commands()).HasValue());
        fixture.Commands().End();
        CHECK(fixture.Device().MemoryUsage().textureCount == 1);

        graph.ReleaseTransientResources();
        fixture.Device().WaitIdle();
        CHECK(fixture.Device().MemoryUsage().textureCount == 0);
    }

    TEST_CASE("an undeclared access is reported and yields no resource") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto declared = graph.CreateTexture(ColorTargetSpec("declared"));
        const auto sneaky = graph.CreateTexture(ColorTargetSpec("sneaky"));
        graph.MarkOutput(declared);

        bool sawNull = false;
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) { builder.Write(declared); },
            [&](PassContext& context) {
                // `sneaky` was never declared by this pass.
                sawNull = context.Texture(sneaky) == nullptr;
                CHECK(context.HasTexture(declared));
                CHECK_FALSE(context.HasTexture(sneaky));
            });

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        fixture.Commands().Begin();
        REQUIRE(graph.Execute(fixture.Device(), fixture.Commands()).HasValue());
        fixture.Commands().End();

        CHECK(sawNull);
        CHECK(graph.ValidationErrorCount() == 1);
        REQUIRE_FALSE(graph.ValidationErrors().empty());
        CHECK(graph.ValidationErrors()[0].find("undeclared") != std::string::npos);
    }

    TEST_CASE("compute and copy passes execute in dependency order") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto instanceData = graph.CreateBuffer(StorageBufferSpec("instance-data"));
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        graph.MarkOutput(color);

        std::vector<std::string> order;
        graph.AddRasterPass(
            "draw-instanced",
            [&](PassBuilder& builder) {
                builder.Read(instanceData);
                builder.Write(color);
            },
            [&](PassContext& context) {
                order.push_back(context.PassName());
                CHECK(context.Buffer(instanceData) != nullptr);
            });
        graph.AddComputePass(
            "cull",
            [&](PassBuilder& builder) { builder.Write(instanceData); },
            [&](PassContext& context) {
                order.push_back(context.PassName());
                context.Commands().BindPipeline(fixture.ComputePipeline());
                context.Commands().Dispatch(4, 1, 1);
            });

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        fixture.Commands().Begin();
        REQUIRE(graph.Execute(fixture.Device(), fixture.Commands()).HasValue());
        fixture.Commands().End();

        REQUIRE(order.size() == 2);
        CHECK(order[0] == "cull");
        CHECK(order[1] == "draw-instanced");
        CHECK(fixture.Commands().GetStats().dispatches == 1);
        CHECK(fixture.Device().MemoryUsage().bufferCount == 1);
    }

    TEST_CASE("passes with debug labels keep the command buffer balanced") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        const auto depth = graph.CreateTexture(DepthTargetSpec("depth"));
        graph.MarkOutput(color);
        GraphFixture::FramebufferCache cache;
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) {
                builder.Write(color);
                builder.Write(depth);
            },
            [&](PassContext& context) { fixture.RecordDraw(context, color, depth, cache); });

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        fixture.Commands().Begin();
        REQUIRE(graph.Execute(fixture.Device(), fixture.Commands()).HasValue());
        fixture.Commands().End();
        // End() succeeded without an "unbalanced debug labels" error.
        CHECK(fixture.RhiErrors() == 0);
    }

    TEST_CASE("a failing transient allocation surfaces as an error") {
        GraphFixture fixture;
        RenderGraph graph;
        TextureSpec impossible = ColorTargetSpec("impossible");
        impossible.width = 1u << 30; // Far beyond the null device's limit.
        impossible.height = 1u << 30;
        const auto huge = graph.CreateTexture(impossible);
        graph.MarkOutput(huge);
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) { builder.Write(huge); },
            [](PassContext&) {});

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        fixture.Commands().Begin();
        auto executed = graph.Execute(fixture.Device(), fixture.Commands());
        fixture.Commands().End();
        REQUIRE(executed.IsError());
        CHECK(executed.Error().Code() == StatusCode::OutOfRange);
        CHECK(graph.ValidationErrorCount() >= 1);
    }

    TEST_CASE("ClearPasses keeps resources so a graph can be rebuilt") {
        RenderGraph graph;
        const auto color = graph.CreateTexture(ColorTargetSpec("color"));
        graph.MarkOutput(color);
        graph.AddRasterPass(
            "scene",
            [&](PassBuilder& builder) { builder.Write(color); },
            [](PassContext&) {});
        REQUIRE(graph.Compile().HasValue());
        CHECK(graph.Stats().declaredPasses == 1);

        graph.ClearPasses();
        CHECK(graph.Stats().executedPasses == 0);
        CHECK(graph.ExecutionOrder().empty());
        // The resource survived, so a second build does not reallocate handles.
        CHECK(graph.Texture(color).name == "color");
        CHECK(graph.Texture(color).firstUse == kInvalidHandle);
        REQUIRE(graph.Compile().HasValue());
        CHECK(graph.Stats().declaredPasses == 0);
    }

    TEST_CASE("two resources with the same description do not alias") {
        GraphFixture fixture;
        RenderGraph graph;
        const auto first = graph.CreateTexture(ColorTargetSpec("first"));
        const auto second = graph.CreateTexture(ColorTargetSpec("second"));
        const auto output = graph.CreateTexture(ColorTargetSpec("output"));
        graph.MarkOutput(output);

        const ITexture* firstPointer = nullptr;
        const ITexture* secondPointer = nullptr;
        graph.AddRasterPass(
            "produce",
            [&](PassBuilder& builder) {
                builder.Write(first);
                builder.Write(second);
            },
            [](PassContext&) {});
        graph.AddRasterPass(
            "consume",
            [&](PassBuilder& builder) {
                builder.Read(first);
                builder.Read(second);
                builder.Write(output);
            },
            [&](PassContext& context) {
                firstPointer = context.Texture(first);
                secondPointer = context.Texture(second);
            });

        auto compiled = graph.Compile();
        REQUIRE(compiled.HasValue());
        fixture.Commands().Begin();
        REQUIRE(graph.Execute(fixture.Device(), fixture.Commands()).HasValue());
        fixture.Commands().End();

        REQUIRE(firstPointer != nullptr);
        REQUIRE(secondPointer != nullptr);
        CHECK(firstPointer != secondPointer);
        CHECK(fixture.Device().MemoryUsage().textureCount == 3);
    }
}
