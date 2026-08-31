// RHI tests.  The null backend is the reference implementation of the RHI
// contract, so these tests double as the specification: resource creation rules,
// command buffer validation, deferred destruction and frame accounting.
#include "doctest.h"

#include "local3d/core/Enum.hpp"
#include "local3d/rhi/RhiDevice.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::rhi;

namespace {

[[nodiscard]] std::unique_ptr<IDevice> MakeNullDevice(u32 frameCount = 2) {
    DeviceDesc desc;
    desc.preferredBackend = BackendType::Null;
    desc.enableValidation = true;
    desc.frameCount = frameCount;
    auto result = CreateDevice(desc);
    REQUIRE_MESSAGE(result.HasValue(), "Failed to create the null device");
    return std::move(*result);
}

/// Advance frames so that deferred deletions can retire.
void AdvanceFrames(IDevice& device, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        device.BeginFrame();
        device.EndFrame();
    }
}

[[nodiscard]] ConstByteSpan AsBytesOf(const std::vector<u8>& data) {
    return std::as_bytes(std::span(data.data(), data.size()));
}

/// The null backend only checks that bytecode exists, not that it is valid.
[[nodiscard]] std::vector<u8> FakeBytecode(usize size = 64) { return std::vector<u8>(size, 0x42); }

[[nodiscard]] ShaderModuleDesc MakeShader(ShaderStage stage, const std::vector<u8>& bytecode,
                                          std::string name) {
    ShaderModuleDesc desc;
    desc.stage = stage;
    desc.bytecode = AsBytesOf(bytecode);
    desc.debugName = std::move(name);
    return desc;
}

[[nodiscard]] BufferDesc VertexBufferDesc(u64 size) {
    BufferDesc desc;
    desc.size = size;
    desc.usage = BufferUsage::Vertex;
    desc.debugName = "vertex-buffer";
    return desc;
}

} // namespace

TEST_SUITE("rhi.device") {
    TEST_CASE("creates the null backend and reports its capabilities") {
        auto device = MakeNullDevice();
        const DeviceInfo& info = device->Info();
        CHECK(info.backend == BackendType::Null);
        CHECK(info.maxTextureSize2D >= 4096);
        CHECK(info.supportsTimestampQueries);
        CHECK(info.supportsIndirectDraw);
        CHECK(device->FrameCount() == 2);
        CHECK(BackendTypeName(BackendType::Vulkan) == "vulkan");
        CHECK(BackendTypeName(BackendType::Null) == "null");
    }

    TEST_CASE("falls back to null when an unavailable backend is requested") {
        DeviceDesc desc;
        desc.preferredBackend = BackendType::Metal;
        bool usedFallback = false;
        auto result = CreateDevice(desc, &usedFallback);
        REQUIRE(result.HasValue());
        CHECK(usedFallback);
        CHECK((*result)->Info().backend == BackendType::Null);
    }

    TEST_CASE("frame index cycles within the frame count") {
        auto device = MakeNullDevice();
        const u32 first = device->FrameIndex();
        AdvanceFrames(*device, 1);
        CHECK(device->FrameIndex() != first);
        AdvanceFrames(*device, 1);
        CHECK(device->FrameIndex() == first);
        CHECK(device->FrameNumber() == 2);
    }

    TEST_CASE("creation failures are returned, misuse is recorded as a violation") {
        auto device = MakeNullDevice();
        // A rejected descriptor is an error result, not a validation entry: the
        // caller has the Status and no GPU work was requested.
        BufferDesc zero;
        CHECK(device->CreateBuffer(zero).IsError());
        BufferDesc noUsage;
        noUsage.size = 64;
        CHECK(device->CreateBuffer(noUsage).IsError());
        CHECK(device->ValidationErrorCount() == 0);

        // Recording a command outside Begin/End is a contract violation.
        auto commandBuffer = device->CreateCommandBuffer();
        REQUIRE(commandBuffer.HasValue());
        Viewport viewport;
        viewport.width = 128.0f;
        viewport.height = 128.0f;
        (*commandBuffer)->SetViewport(viewport);
        CHECK(device->ValidationErrorCount() == 1);
    }
}

TEST_SUITE("rhi.formats") {
    TEST_CASE("format table describes every format") {
        CHECK(GetFormatInfo(Format::RGBA8_UNorm).bytesPerBlock == 4);
        CHECK(GetFormatInfo(Format::RGBA16_Float).bytesPerBlock == 8);
        CHECK(GetFormatInfo(Format::Depth32_Float).hasDepth);
        CHECK(GetFormatInfo(Format::Depth24_Stencil8).hasStencil);
        CHECK(GetFormatInfo(Format::RGBA8_SRGB).isSrgb);
        CHECK(GetFormatInfo(Format::BC7_RGBA_UNorm).isCompressed);
        CHECK(GetFormatInfo(Format::BC7_RGBA_UNorm).blockWidth == 4);
        CHECK(IsDepthFormat(Format::Depth16_UNorm));
        CHECK_FALSE(IsDepthFormat(Format::RGBA8_UNorm));
        CHECK(GetFormatInfo(static_cast<Format>(9999)).name == "Unknown");
    }

    TEST_CASE("mip size computation handles compressed and odd sizes") {
        CHECK(ComputeMipBytes(Format::RGBA8_UNorm, 256, 256, 0) == 256 * 256 * 4);
        CHECK(ComputeMipBytes(Format::RGBA8_UNorm, 256, 256, 1) == 128 * 128 * 4);
        CHECK(ComputeMipBytes(Format::RGBA8_UNorm, 3, 5, 0) == 3 * 5 * 4);
        // BC7 packs 4x4 blocks, so a 1x1 mip still costs one 16 byte block.
        CHECK(ComputeMipBytes(Format::BC7_RGBA_UNorm, 1, 1, 0) == 16);
        CHECK(ComputeMipBytes(Format::BC7_RGBA_UNorm, 8, 8, 0) == 4 * 16);
        CHECK(MipLevelSize(1, 5) == 1);
    }
}

TEST_SUITE("rhi.buffers") {
    TEST_CASE("creates a buffer and uploads data into it") {
        auto device = MakeNullDevice();
        BufferDesc desc = VertexBufferDesc(256);
        desc.usage = BufferUsage::Vertex | BufferUsage::CopyDestination;
        auto buffer = device->CreateBuffer(desc);
        REQUIRE(buffer.HasValue());
        CHECK((*buffer)->Size() == 256);
        CHECK((*buffer)->DebugName() == "vertex-buffer");
        CHECK(HasAllFlags((*buffer)->Usage(), BufferUsage::Vertex));
        CHECK((*buffer)->Memory() == MemoryType::GpuOnly);

        const std::vector<u8> data(64, 0xAB);
        device->UpdateBuffer(**buffer, 0, AsBytesOf(data));
        CHECK(device->ValidationErrorCount() == 0);

        const auto report = device->MemoryUsage();
        CHECK(report.bufferCount == 1);
        CHECK(report.bufferBytes == 256);
        CHECK(report.totalAllocatedBytes == 256);
    }

    TEST_CASE("rejects invalid buffer descriptions") {
        auto device = MakeNullDevice();
        BufferDesc zero;
        zero.usage = BufferUsage::Vertex;
        CHECK(device->CreateBuffer(zero).IsError());

        BufferDesc noUsage;
        noUsage.size = 64;
        CHECK(device->CreateBuffer(noUsage).IsError());

        BufferDesc huge;
        huge.size = 1ULL << 62;
        huge.usage = BufferUsage::Storage;
        auto result = device->CreateBuffer(huge);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::OutOfRange);
    }

    TEST_CASE("out of range uploads are reported as validation errors") {
        auto device = MakeNullDevice();
        BufferDesc desc = VertexBufferDesc(32);
        desc.usage = BufferUsage::Uniform | BufferUsage::CopyDestination;
        auto buffer = device->CreateBuffer(desc);
        REQUIRE(buffer.HasValue());

        const std::vector<u8> data(64, 0x01);
        device->UpdateBuffer(**buffer, 0, AsBytesOf(data));
        REQUIRE(device->ValidationErrorCount() == 1);
        CHECK(device->ValidationErrors()[0].find("past the end") != std::string::npos);
    }

    TEST_CASE("destroying a buffer defers the free until frames complete") {
        auto device = MakeNullDevice();
        {
            auto buffer = device->CreateBuffer(VertexBufferDesc(512));
            REQUIRE(buffer.HasValue());
            CHECK(device->MemoryUsage().bufferCount == 1);
        } // ResourcePtr releases here.

        // Still alive: the GPU may read it for frameCount more frames.
        AdvanceFrames(*device, 2);
        CHECK(device->MemoryUsage().bufferCount == 1);
        CHECK(device->MemoryUsage().deferredResources == 1);

        AdvanceFrames(*device, 1);
        CHECK(device->MemoryUsage().bufferCount == 0);
        CHECK(device->MemoryUsage().deferredResources == 0);
        CHECK(device->MemoryUsage().totalFreedBytes == 512);
    }

    TEST_CASE("wait idle releases everything immediately") {
        auto device = MakeNullDevice();
        {
            auto buffer = device->CreateBuffer(VertexBufferDesc(128));
            REQUIRE(buffer.HasValue());
        }
        CHECK(device->MemoryUsage().bufferCount == 1);
        device->WaitIdle();
        CHECK(device->MemoryUsage().bufferCount == 0);
        // WaitIdle must not disturb the frame pacing.
        CHECK(device->FrameNumber() == 0);
    }

    TEST_CASE("host visible buffers can be mapped") {
        auto device = MakeNullDevice();
        BufferDesc desc = VertexBufferDesc(64);
        desc.usage = BufferUsage::Vertex | BufferUsage::CopyDestination;
        desc.memory = MemoryType::CpuToGpu;
        auto buffer = device->CreateBuffer(desc);
        REQUIRE(buffer.HasValue());
        void* mapped = (*buffer)->Map();
        REQUIRE(mapped != nullptr);
        std::memcpy(mapped, "hello", 6);
        CHECK(std::memcmp(mapped, "hello", 6) == 0);
        (*buffer)->Unmap();
    }
}

TEST_SUITE("rhi.textures") {
    TEST_CASE("creates a texture and computes its footprint") {
        auto device = MakeNullDevice();
        TextureDesc desc;
        desc.width = 512;
        desc.height = 512;
        desc.mipLevels = 10;
        desc.format = Format::RGBA8_UNorm;
        desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
        desc.debugName = "albedo";
        auto texture = device->CreateTexture(desc);
        REQUIRE(texture.HasValue());
        CHECK((*texture)->Width() == 512);
        // Exact sum of the 10 mip levels: 1048576 + 262144 + ... + 4 = 1398100.
        CHECK((*texture)->SizeInBytes() == 1398100);
        CHECK(device->MemoryUsage().textureCount == 1);
        CHECK(device->MemoryUsage().textureBytes == 1398100);
    }

    TEST_CASE("validates texture descriptions") {
        auto device = MakeNullDevice();
        TextureDesc base;
        base.width = 64;
        base.height = 64;
        base.format = Format::RGBA8_UNorm;
        base.usage = TextureUsage::Sampled;

        TextureDesc tooManyMips = base;
        tooManyMips.mipLevels = 32;
        CHECK(device->CreateTexture(tooManyMips).IsError());

        TextureDesc noFormat = base;
        noFormat.format = Format::Unknown;
        CHECK(device->CreateTexture(noFormat).IsError());

        TextureDesc noUsage = base;
        noUsage.usage = TextureUsage::None;
        CHECK(device->CreateTexture(noUsage).IsError());

        TextureDesc badCube = base;
        badCube.dimension = TextureDimension::Cube;
        badCube.arrayLayers = 4;
        CHECK(device->CreateTexture(badCube).IsError());

        TextureDesc goodCube = base;
        goodCube.dimension = TextureDimension::Cube;
        goodCube.arrayLayers = 6;
        CHECK(device->CreateTexture(goodCube).HasValue());
    }

    TEST_CASE("mip uploads validate sizes and keep the data") {
        auto device = MakeNullDevice();
        TextureDesc desc;
        desc.width = 4;
        desc.height = 4;
        desc.mipLevels = 3;
        desc.format = Format::RGBA8_UNorm;
        desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
        auto texture = device->CreateTexture(desc);
        REQUIRE(texture.HasValue());

        std::vector<u8> mip0(4 * 4 * 4, 0x11);
        std::vector<u8> mip1(2 * 2 * 4, 0x22);
        std::vector<u8> mip2(1 * 1 * 4, 0x33);
        const std::array<ConstByteSpan, 3> mips{AsBytesOf(mip0), AsBytesOf(mip1), AsBytesOf(mip2)};
        device->UpdateTexture(**texture, mips);
        CHECK(device->ValidationErrorCount() == 0);

        // A too-short mip chain must be rejected.
        std::vector<u8> shortMip0(10, 0x11);
        const std::array<ConstByteSpan, 3> badMips{AsBytesOf(shortMip0), AsBytesOf(mip1),
                                                   AsBytesOf(mip2)};
        device->UpdateTexture(**texture, badMips);
        CHECK(device->ValidationErrorCount() == 1);
    }

    TEST_CASE("depth textures are created as attachments") {
        auto device = MakeNullDevice();
        TextureDesc desc;
        desc.width = 128;
        desc.height = 128;
        desc.format = Format::Depth32_Float;
        desc.usage = TextureUsage::DepthStencilAttachment;
        auto texture = device->CreateTexture(desc);
        REQUIRE(texture.HasValue());
        CHECK(IsDepthFormat((*texture)->GetFormat()));
    }
}

TEST_SUITE("rhi.shaders_and_samplers") {
    TEST_CASE("shader modules keep their bytecode") {
        auto device = MakeNullDevice();
        const auto code = FakeBytecode(128);
        auto module = device->CreateShaderModule(MakeShader(ShaderStage::Fragment, code, "fs"));
        REQUIRE(module.HasValue());
        CHECK((*module)->Stage() == ShaderStage::Fragment);
        CHECK((*module)->BytecodeSize() == 128);
        CHECK((*module)->GetFormat() == ShaderFormat::Spirv);

        ShaderModuleDesc empty;
        empty.stage = ShaderStage::Vertex;
        CHECK(device->CreateShaderModule(empty).IsError());
    }

    TEST_CASE("samplers carry their filter settings") {
        auto device = MakeNullDevice();
        SamplerDesc desc;
        desc.minFilter = Filter::Linear;
        desc.maxAnisotropy = 8.0f;
        auto sampler = device->CreateSampler(desc);
        REQUIRE(sampler.HasValue());
        CHECK((*sampler)->Desc().maxAnisotropy == doctest::Approx(8.0f));
    }
}

TEST_SUITE("rhi.pipelines") {
    TEST_CASE("graphics pipeline requires vertex and fragment stages") {
        auto device = MakeNullDevice();
        const auto vertexCode = FakeBytecode();
        const auto fragmentCode = FakeBytecode();

        PipelineDesc valid;
        valid.shaders.PushBack(MakeShader(ShaderStage::Vertex, vertexCode, "vs"));
        valid.shaders.PushBack(MakeShader(ShaderStage::Fragment, fragmentCode, "fs"));
        valid.colorFormats.PushBack(Format::RGBA16_Float);
        valid.depthFormat = Format::Depth32_Float;
        valid.hasDepthAttachment = true;
        valid.debugName = "pbr-opaque";

        auto pipeline = device->CreateGraphicsPipeline(valid);
        CHECK(pipeline.HasValue());
        CHECK(device->ValidationErrorCount() == 0);
        CHECK_FALSE((*pipeline)->IsCompute());

        PipelineDesc missingFragment;
        missingFragment.shaders.PushBack(MakeShader(ShaderStage::Vertex, vertexCode, "vs"));
        missingFragment.colorFormats.PushBack(Format::RGBA16_Float);
        auto incomplete = device->CreateGraphicsPipeline(missingFragment);
        REQUIRE(incomplete.HasValue());
        CHECK(device->ValidationErrorCount() == 1);
    }

    TEST_CASE("push constant ranges are bounded by the device limit") {
        auto device = MakeNullDevice();
        const auto vertexCode = FakeBytecode();
        const auto fragmentCode = FakeBytecode();

        PipelineDesc desc;
        desc.shaders.PushBack(MakeShader(ShaderStage::Vertex, vertexCode, "vs"));
        desc.shaders.PushBack(MakeShader(ShaderStage::Fragment, fragmentCode, "fs"));
        desc.colorFormats.PushBack(Format::RGBA16_Float);
        PushConstantRange oversized;
        oversized.stage = ShaderStage::Vertex;
        oversized.size = 4096;
        desc.pushConstantRanges.PushBack(oversized);

        auto pipeline = device->CreateGraphicsPipeline(desc);
        REQUIRE(pipeline.HasValue());
        REQUIRE(device->ValidationErrorCount() == 1);
        CHECK(device->ValidationErrors()[0].find("Push constant") != std::string::npos);
    }

    TEST_CASE("vertex attributes must reference a declared binding") {
        auto device = MakeNullDevice();
        const auto vertexCode = FakeBytecode();
        const auto fragmentCode = FakeBytecode();

        PipelineDesc desc;
        desc.shaders.PushBack(MakeShader(ShaderStage::Vertex, vertexCode, "vs"));
        desc.shaders.PushBack(MakeShader(ShaderStage::Fragment, fragmentCode, "fs"));
        desc.colorFormats.PushBack(Format::RGBA16_Float);
        VertexAttribute orphan;
        orphan.binding = 3;
        desc.vertexAttributes.PushBack(orphan);

        auto pipeline = device->CreateGraphicsPipeline(desc);
        REQUIRE(pipeline.HasValue());
        CHECK(device->ValidationErrorCount() == 1);
    }

    TEST_CASE("compute pipelines need a compute shader") {
        auto device = MakeNullDevice();
        const auto computeCode = FakeBytecode();
        PipelineDesc desc;
        desc.shaders.PushBack(MakeShader(ShaderStage::Compute, computeCode, "cs"));
        auto pipeline = device->CreateComputePipeline(desc);
        REQUIRE(pipeline.HasValue());
        CHECK((*pipeline)->IsCompute());
        CHECK(device->ValidationErrorCount() == 0);

        const auto vertexCode = FakeBytecode();
        PipelineDesc wrong;
        wrong.shaders.PushBack(MakeShader(ShaderStage::Vertex, vertexCode, "vs"));
        auto invalid = device->CreateComputePipeline(wrong);
        REQUIRE(invalid.HasValue());
        CHECK(device->ValidationErrorCount() == 1);
    }
}

TEST_SUITE("rhi.render_passes") {
    TEST_CASE("framebuffers must match the pass attachments") {
        auto device = MakeNullDevice();
        TextureDesc colorDesc;
        colorDesc.width = 256;
        colorDesc.height = 128;
        colorDesc.format = Format::RGBA16_Float;
        colorDesc.usage = TextureUsage::ColorAttachment;
        auto color = device->CreateTexture(colorDesc);
        REQUIRE(color.HasValue());

        TextureDesc depthDesc = colorDesc;
        depthDesc.format = Format::Depth32_Float;
        depthDesc.usage = TextureUsage::DepthStencilAttachment;
        auto depth = device->CreateTexture(depthDesc);
        REQUIRE(depth.HasValue());

        RenderPassDesc passDesc;
        AttachmentDesc colorAttachment;
        colorAttachment.format = Format::RGBA16_Float;
        passDesc.colorAttachments.PushBack(colorAttachment);
        passDesc.hasDepthStencil = true;
        passDesc.depthStencil.format = Format::Depth32_Float;
        auto pass = device->CreateRenderPass(passDesc);
        REQUIRE(pass.HasValue());

        std::array<const ITexture* const, 2> attachments{color->get(), depth->get()};
        auto framebuffer = device->CreateFramebuffer(**pass, attachments, 256, 128, "main");
        CHECK(framebuffer.HasValue());
        CHECK((*framebuffer)->AttachmentCount() == 2);
        CHECK((*framebuffer)->Width() == 256);
        CHECK(&(*framebuffer)->RenderPass() == pass->get());

        // Wrong extent.
        CHECK(device->CreateFramebuffer(**pass, attachments, 512, 512, "wrong").IsError());

        // Missing depth attachment.
        std::array<const ITexture* const, 1> onlyColor{color->get()};
        CHECK(device->CreateFramebuffer(**pass, onlyColor, 256, 128, "missing").IsError());
    }

    TEST_CASE("a render pass needs at least one attachment") {
        auto device = MakeNullDevice();
        RenderPassDesc empty;
        CHECK(device->CreateRenderPass(empty).IsError());
    }
}

TEST_SUITE("rhi.descriptors") {
    TEST_CASE("layouts and sets validate writes") {
        auto device = MakeNullDevice();
        DescriptorSetLayoutDesc layoutDesc;
        DescriptorBinding binding;
        binding.type = DescriptorType::UniformBuffer;
        layoutDesc.bindings.PushBack(binding);
        auto layout = device->CreateDescriptorSetLayout(layoutDesc);
        REQUIRE(layout.HasValue());
        CHECK((*layout)->LayoutId() == (*layout)->ResourceId());

        auto set = device->AllocateDescriptorSet(**layout);
        REQUIRE(set.HasValue());
        CHECK(&(*set)->Layout() == layout->get());

        auto buffer = device->CreateBuffer(VertexBufferDesc(256));
        REQUIRE(buffer.HasValue());

        DescriptorWrite write;
        write.type = DescriptorType::UniformBuffer;
        write.buffer = buffer->get();
        write.bufferRange = 256;
        const std::array<DescriptorWrite, 1> writes{write};
        device->WriteDescriptorSet(**set, writes);
        CHECK(device->ValidationErrorCount() == 0);
        CHECK((*set)->WriteCount() == 1);

        DescriptorWrite wrongBinding;
        wrongBinding.binding = 7;
        wrongBinding.type = DescriptorType::UniformBuffer;
        wrongBinding.buffer = buffer->get();
        const std::array<DescriptorWrite, 1> badWrites{wrongBinding};
        device->WriteDescriptorSet(**set, badWrites);
        CHECK(device->ValidationErrorCount() == 1);

        DescriptorWrite wrongType;
        wrongType.type = DescriptorType::SampledImage;
        wrongType.texture = nullptr;
        const std::array<DescriptorWrite, 1> typeWrites{wrongType};
        device->WriteDescriptorSet(**set, typeWrites);
        CHECK(device->ValidationErrorCount() == 2);
    }

    TEST_CASE("an empty layout is rejected") {
        auto device = MakeNullDevice();
        DescriptorSetLayoutDesc empty;
        CHECK(device->CreateDescriptorSetLayout(empty).IsError());
    }
}

namespace {

/// Everything needed to record a valid graphics pass, built once per test case.
class GraphicsFixture {
public:
    GraphicsFixture() : device_(MakeNullDevice()) {
        const auto vertexCode = FakeBytecode();
        const auto fragmentCode = FakeBytecode();

        DescriptorSetLayoutDesc layoutDesc;
        DescriptorBinding binding;
        binding.type = DescriptorType::UniformBuffer;
        layoutDesc.bindings.PushBack(binding);
        auto layout = device_->CreateDescriptorSetLayout(layoutDesc);
        REQUIRE(layout.HasValue());
        layout_ = std::move(*layout);

        TextureDesc colorDesc;
        colorDesc.width = 256;
        colorDesc.height = 256;
        colorDesc.format = Format::RGBA16_Float;
        colorDesc.usage = TextureUsage::ColorAttachment;
        auto color = device_->CreateTexture(colorDesc);
        REQUIRE(color.HasValue());
        color_ = std::move(*color);

        TextureDesc depthDesc = colorDesc;
        depthDesc.format = Format::Depth32_Float;
        depthDesc.usage = TextureUsage::DepthStencilAttachment;
        auto depth = device_->CreateTexture(depthDesc);
        REQUIRE(depth.HasValue());
        depth_ = std::move(*depth);

        RenderPassDesc passDesc;
        AttachmentDesc colorAttachment;
        colorAttachment.format = Format::RGBA16_Float;
        passDesc.colorAttachments.PushBack(colorAttachment);
        passDesc.hasDepthStencil = true;
        passDesc.depthStencil.format = Format::Depth32_Float;
        auto pass = device_->CreateRenderPass(passDesc);
        REQUIRE(pass.HasValue());
        pass_ = std::move(*pass);

        std::array<const ITexture* const, 2> attachments{color_.get(), depth_.get()};
        auto framebuffer = device_->CreateFramebuffer(*pass_, attachments, 256, 256, "target");
        REQUIRE(framebuffer.HasValue());
        framebuffer_ = std::move(*framebuffer);

        auto vertexBuffer = device_->CreateBuffer(VertexBufferDesc(1024));
        REQUIRE(vertexBuffer.HasValue());
        vertexBuffer_ = std::move(*vertexBuffer);

        BufferDesc indexDesc;
        indexDesc.size = 1024;
        indexDesc.usage = BufferUsage::Index;
        auto indexBuffer = device_->CreateBuffer(indexDesc);
        REQUIRE(indexBuffer.HasValue());
        indexBuffer_ = std::move(*indexBuffer);

        PipelineDesc pipelineDesc;
        pipelineDesc.shaders.PushBack(MakeShader(ShaderStage::Vertex, vertexCode, "vs"));
        pipelineDesc.shaders.PushBack(MakeShader(ShaderStage::Fragment, fragmentCode, "fs"));
        pipelineDesc.colorFormats.PushBack(Format::RGBA16_Float);
        pipelineDesc.depthFormat = Format::Depth32_Float;
        pipelineDesc.hasDepthAttachment = true;
        pipelineDesc.descriptorSetLayoutIds.PushBack(static_cast<u32>(layout_->LayoutId()));
        PushConstantRange pushConstants;
        pushConstants.stage = ShaderStage::Vertex;
        pushConstants.size = 64;
        pipelineDesc.pushConstantRanges.PushBack(pushConstants);
        VertexBinding vertexBinding;
        vertexBinding.stride = 32;
        pipelineDesc.vertexBindings.PushBack(vertexBinding);
        VertexAttribute position;
        position.format = Format::RGBA32_Float;
        pipelineDesc.vertexAttributes.PushBack(position);
        auto pipeline = device_->CreateGraphicsPipeline(pipelineDesc);
        REQUIRE(pipeline.HasValue());
        pipeline_ = std::move(*pipeline);

        auto set = device_->AllocateDescriptorSet(*layout_);
        REQUIRE(set.HasValue());
        descriptorSet_ = std::move(*set);

        auto commandBuffer = device_->CreateCommandBuffer();
        REQUIRE(commandBuffer.HasValue());
        commandBuffer_ = std::move(*commandBuffer);
    }

    [[nodiscard]] IDevice& Device() { return *device_; }
    [[nodiscard]] ICommandBuffer& Commands() { return *commandBuffer_; }
    [[nodiscard]] const ICommandBuffer::Stats& Stats() const { return commandBuffer_->GetStats(); }
    [[nodiscard]] usize Errors() const { return device_->ValidationErrorCount(); }
    [[nodiscard]] std::string LastError() const {
        const auto errors = device_->ValidationErrors();
        return errors.empty() ? std::string{} : errors.back();
    }
    [[nodiscard]] IPipeline& Pipeline() { return *pipeline_; }
    [[nodiscard]] IDescriptorSet& DescriptorSet() { return *descriptorSet_; }
    [[nodiscard]] IDescriptorSetLayout& Layout() { return *layout_; }
    [[nodiscard]] IRenderPass& Pass() { return *pass_; }
    [[nodiscard]] IFramebuffer& Framebuffer() { return *framebuffer_; }
    [[nodiscard]] IBuffer& VertexBuffer() { return *vertexBuffer_; }
    [[nodiscard]] IBuffer& IndexBuffer() { return *indexBuffer_; }

    /// Begins the command buffer and opens the render pass, no draw state yet.
    void BeginPass() {
        std::array<ClearValue, 1> clearValues{};
        Commands().Begin("opaque-pass");
        Commands().BeginRenderPass(*pass_, *framebuffer_, clearValues);
    }

    /// Records a valid render pass up to (and including) the draw state.
    void BeginValidPass() {
        BeginPass();
        Commands().BindPipeline(*pipeline_);
        Viewport viewport;
        viewport.width = 256.0f;
        viewport.height = 256.0f;
        Commands().SetViewport(viewport);
        Commands().BindVertexBuffer(0, *vertexBuffer_);
        Commands().BindIndexBuffer(*indexBuffer_, 0, false);
    }

private:
    std::unique_ptr<IDevice> device_;
    DescriptorSetLayoutPtr layout_;
    TexturePtr color_;
    TexturePtr depth_;
    RenderPassPtr pass_;
    FramebufferPtr framebuffer_;
    BufferPtr vertexBuffer_;
    BufferPtr indexBuffer_;
    PipelinePtr pipeline_;
    DescriptorSetPtr descriptorSet_;
    CommandBufferPtr commandBuffer_;
};

} // namespace

TEST_SUITE("rhi.command_buffer") {
    TEST_CASE("records a complete pass and accounts the work") {
        GraphicsFixture fixture;
        fixture.Device().BeginFrame();

        fixture.BeginValidPass();
        CHECK(fixture.Errors() == 0);
        CHECK(fixture.Commands().IsInsideRenderPass());

        std::array<u32, 4> constants{1, 2, 3, 4};
        const ConstByteSpan pushBytes =
            std::as_bytes(std::span(constants.data(), constants.size()));
        fixture.Commands().PushConstants(ShaderStage::Vertex, 0, pushBytes);
        fixture.Commands().BindDescriptorSet(0, fixture.Layout(), fixture.DescriptorSet());
        fixture.Commands().BeginDebugLabel("opaque geometry");
        fixture.Commands().DrawIndexed(36, 1);
        fixture.Commands().Draw(3);
        fixture.Commands().EndDebugLabel();
        fixture.Commands().EndRenderPass();
        CHECK_FALSE(fixture.Commands().IsInsideRenderPass());
        fixture.Commands().End();

        CHECK(fixture.Errors() == 0);
        const auto& stats = fixture.Stats();
        CHECK(stats.renderPasses == 1);
        CHECK(stats.pipelineBinds == 1);
        CHECK(stats.indexedDrawCalls == 1);
        CHECK(stats.drawCalls == 1);
        CHECK(stats.pushConstantWrites == 1);
        CHECK(stats.descriptorSetBinds == 1);

        fixture.Device().Submit(fixture.Commands());
        CHECK(fixture.Errors() == 0);
    }

    TEST_CASE("recording outside Begin/End is rejected") {
        GraphicsFixture fixture;
        fixture.Commands().Draw(3);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("outside Begin/End") != std::string::npos);
    }

    TEST_CASE("drawing outside a render pass is rejected") {
        GraphicsFixture fixture;
        fixture.Commands().Begin();
        fixture.Commands().Draw(3);
        CHECK(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("outside a render pass") != std::string::npos);
    }

    TEST_CASE("drawing without a pipeline is rejected") {
        GraphicsFixture fixture;
        fixture.BeginPass();
        fixture.Commands().Draw(3);
        CHECK(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("without a pipeline bound") != std::string::npos);
    }

    TEST_CASE("drawing before SetViewport is rejected") {
        GraphicsFixture fixture;
        fixture.BeginPass();
        fixture.Commands().BindPipeline(fixture.Pipeline());
        fixture.Commands().Draw(3);
        CHECK(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("before SetViewport") != std::string::npos);
    }

    TEST_CASE("drawing with an unbound vertex binding is rejected") {
        GraphicsFixture fixture;
        fixture.BeginPass();
        fixture.Commands().BindPipeline(fixture.Pipeline());
        Viewport viewport;
        viewport.width = 256.0f;
        viewport.height = 256.0f;
        fixture.Commands().SetViewport(viewport);
        fixture.Commands().Draw(3);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("Vertex binding 0") != std::string::npos);
    }

    TEST_CASE("an indexed draw without an index buffer is rejected") {
        GraphicsFixture fixture;
        fixture.BeginPass();
        fixture.Commands().BindPipeline(fixture.Pipeline());
        Viewport viewport;
        viewport.width = 256.0f;
        viewport.height = 256.0f;
        fixture.Commands().SetViewport(viewport);
        fixture.Commands().BindVertexBuffer(0, fixture.VertexBuffer());
        fixture.Commands().DrawIndexed(6, 1);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("index buffer") != std::string::npos);
    }

    TEST_CASE("a zero sized draw is rejected") {
        GraphicsFixture fixture;
        fixture.BeginValidPass();
        fixture.Commands().Draw(0, 1);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("zero vertex") != std::string::npos);
    }

    TEST_CASE("buffers must carry the usage flag they are bound as") {
        GraphicsFixture fixture;
        BufferDesc uniformDesc;
        uniformDesc.size = 256;
        uniformDesc.usage = BufferUsage::Uniform;
        auto uniform = fixture.Device().CreateBuffer(uniformDesc);
        REQUIRE(uniform.HasValue());

        fixture.Commands().Begin();
        fixture.Commands().BindVertexBuffer(0, **uniform);
        CHECK(fixture.LastError().find("Vertex usage flag") != std::string::npos);
        fixture.Commands().BindIndexBuffer(**uniform, 0, false);
        CHECK(fixture.LastError().find("Index usage flag") != std::string::npos);
        CHECK(fixture.Errors() == 2);
    }

    TEST_CASE("descriptor sets must match the pipeline layout") {
        GraphicsFixture fixture;
        // A second, unrelated layout.
        DescriptorSetLayoutDesc otherLayoutDesc;
        DescriptorBinding binding;
        binding.type = DescriptorType::SampledImage;
        otherLayoutDesc.bindings.PushBack(binding);
        auto otherLayout = fixture.Device().CreateDescriptorSetLayout(otherLayoutDesc);
        REQUIRE(otherLayout.HasValue());
        auto otherSet = fixture.Device().AllocateDescriptorSet(**otherLayout);
        REQUIRE(otherSet.HasValue());

        fixture.BeginValidPass();
        fixture.Commands().BindDescriptorSet(0, **otherLayout, **otherSet);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("pipeline layout") != std::string::npos);

        // An index the pipeline never declared.
        fixture.Commands().BindDescriptorSet(3, fixture.Layout(), fixture.DescriptorSet());
        REQUIRE(fixture.Errors() == 2);
        CHECK(fixture.LastError().find("not declared") != std::string::npos);
    }

    TEST_CASE("push constants must fall inside the declared range") {
        GraphicsFixture fixture;
        std::array<u32, 4> constants{1, 2, 3, 4};
        const ConstByteSpan bytes = std::as_bytes(std::span(constants.data(), constants.size()));

        fixture.BeginValidPass();
        fixture.Commands().PushConstants(ShaderStage::Vertex, 0, bytes);
        CHECK(fixture.Errors() == 0);

        // Past the end of the declared 64 byte range.
        fixture.Commands().PushConstants(ShaderStage::Vertex, 64, bytes);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("declared ranges") != std::string::npos);

        // A stage the pipeline never declared.
        fixture.Commands().PushConstants(ShaderStage::Fragment, 0, bytes);
        REQUIRE(fixture.Errors() == 2);
    }

    TEST_CASE("a compute pipeline cannot be bound inside a render pass") {
        GraphicsFixture fixture;
        const auto computeCode = FakeBytecode();
        PipelineDesc desc;
        desc.shaders.PushBack(MakeShader(ShaderStage::Compute, computeCode, "cs"));
        auto compute = fixture.Device().CreateComputePipeline(desc);
        REQUIRE(compute.HasValue());

        fixture.BeginPass();
        fixture.Commands().BindPipeline(**compute);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("Compute pipeline") != std::string::npos);
    }

    TEST_CASE("compute dispatches happen outside a render pass") {
        GraphicsFixture fixture;
        const auto computeCode = FakeBytecode();
        PipelineDesc desc;
        desc.shaders.PushBack(MakeShader(ShaderStage::Compute, computeCode, "cs"));
        auto compute = fixture.Device().CreateComputePipeline(desc);
        REQUIRE(compute.HasValue());

        fixture.Commands().Begin();
        fixture.Commands().BindPipeline(**compute);
        CHECK(fixture.Errors() == 0);
        fixture.Commands().Dispatch(16, 16, 1);
        CHECK(fixture.Errors() == 0);
        CHECK(fixture.Stats().dispatches == 1);

        fixture.Commands().Dispatch(0, 1, 1);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("zero group count") != std::string::npos);
        fixture.Commands().End();
    }

    TEST_CASE("copies validate usage flags and ranges") {
        GraphicsFixture fixture;
        BufferDesc stagingDesc;
        stagingDesc.size = 512;
        stagingDesc.usage = BufferUsage::CopySource;
        auto staging = fixture.Device().CreateBuffer(stagingDesc);
        REQUIRE(staging.HasValue());

        BufferDesc targetDesc;
        targetDesc.size = 512;
        targetDesc.usage = BufferUsage::CopyDestination | BufferUsage::Uniform;
        auto target = fixture.Device().CreateBuffer(targetDesc);
        REQUIRE(target.HasValue());

        fixture.Commands().Begin();
        fixture.Commands().CopyBuffer(**staging, 0, **target, 0, 512);
        CHECK(fixture.Errors() == 0);
        CHECK(fixture.Stats().copies == 1);

        fixture.Commands().CopyBuffer(**staging, 0, **target, 0, 1024);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("exceeds a buffer") != std::string::npos);

        // Copying into a buffer without the CopyDestination usage.
        fixture.Commands().CopyBuffer(**staging, 0, fixture.VertexBuffer(), 0, 64);
        REQUIRE(fixture.Errors() == 2);
        CHECK(fixture.LastError().find("CopyDestination") != std::string::npos);
        fixture.Commands().End();
    }

    TEST_CASE("submitting a buffer that is still recording is rejected") {
        GraphicsFixture fixture;
        fixture.BeginValidPass();
        fixture.Commands().BeginDebugLabel("unbalanced");
        fixture.Device().Submit(fixture.Commands());
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("still recording") != std::string::npos);

        // Closing it now surfaces the two recording mistakes exactly once each.
        fixture.Commands().End();
        REQUIRE(fixture.Errors() == 3);
    }

    TEST_CASE("only a finished recording can be submitted") {
        GraphicsFixture fixture;
        // Never begun.
        fixture.Device().Submit(fixture.Commands());
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("without a finished recording") != std::string::npos);

        // A valid recording submits once; the same buffer cannot be submitted twice.
        fixture.Commands().Begin();
        fixture.Commands().End();
        fixture.Device().Submit(fixture.Commands());
        CHECK(fixture.Errors() == 1);
        fixture.Device().Submit(fixture.Commands());
        REQUIRE(fixture.Errors() == 2);
        CHECK(fixture.LastError().find("without a finished recording") != std::string::npos);
    }

    TEST_CASE("indirect draws validate the argument buffer") {
        GraphicsFixture fixture;
        BufferDesc argumentDesc;
        argumentDesc.size = 256;
        argumentDesc.usage = BufferUsage::Indirect | BufferUsage::Storage;
        auto arguments = fixture.Device().CreateBuffer(argumentDesc);
        REQUIRE(arguments.HasValue());

        fixture.BeginValidPass();
        fixture.Commands().DrawIndexedIndirect(**arguments, 0, 4, 20);
        CHECK(fixture.Errors() == 0);
        CHECK(fixture.Stats().indirectDrawCalls == 1);

        fixture.Commands().DrawIndexedIndirect(**arguments, 0, 100, 20);
        REQUIRE(fixture.Errors() == 1);
        CHECK(fixture.LastError().find("exceed the buffer size") != std::string::npos);

        // A buffer without the Indirect usage flag.
        fixture.Commands().DrawIndexedIndirect(fixture.VertexBuffer(), 0, 1, 20);
        REQUIRE(fixture.Errors() == 2);
        CHECK(fixture.LastError().find("Indirect usage flag") != std::string::npos);
    }
}

TEST_SUITE("rhi.queries_and_swapchain") {
    TEST_CASE("timestamp queries resolve after the commands are recorded") {
        GraphicsFixture fixture;
        auto pool = fixture.Device().CreateTimestampQueryPool(2);
        REQUIRE(pool.HasValue());
        CHECK((*pool)->QueryCount() == 2);

        fixture.Commands().Begin();
        fixture.Commands().WriteTimestamp(**pool, 0);
        fixture.Commands().WriteTimestamp(**pool, 1);
        fixture.Commands().End();
        CHECK(fixture.Errors() == 0);
        CHECK(fixture.Stats().timestamps == 2);

        const auto timestamps = fixture.Device().ReadTimestamps(**pool, 0, 2);
        REQUIRE(timestamps.size() == 2);
        CHECK(timestamps[0] > 0.0);
        CHECK(timestamps[1] >= timestamps[0]);

        // Out of range indices are rejected rather than reading garbage.
        fixture.Commands().Begin();
        fixture.Commands().WriteTimestamp(**pool, 5);
        CHECK(fixture.Errors() == 1);
        fixture.Commands().End();
        CHECK(fixture.Device().ReadTimestamps(**pool, 0, 8).empty());
    }

    TEST_CASE("a zero sized query pool is rejected") {
        GraphicsFixture fixture;
        auto pool = fixture.Device().CreateTimestampQueryPool(0);
        CHECK(pool.IsError());
    }

    TEST_CASE("the swapchain cycles images and counts presents") {
        GraphicsFixture fixture;
        SwapchainDesc desc;
        desc.width = 1280;
        desc.height = 720;
        desc.imageCount = 3;
        auto swapchain = fixture.Device().CreateSwapchain(desc);
        REQUIRE(swapchain.HasValue());
        CHECK((*swapchain)->ImageCount() == 3);
        CHECK((*swapchain)->Width() == 1280);
        CHECK((*swapchain)->GetFormat() == Format::BGRA8_UNorm);

        bool suboptimal = true;
        REQUIRE((*swapchain)->AcquireNextImage(suboptimal));
        CHECK_FALSE(suboptimal);
        const u32 first = (*swapchain)->CurrentImageIndex();
        REQUIRE((*swapchain)->AcquireNextImage(suboptimal));
        CHECK((*swapchain)->CurrentImageIndex() != first);
        CHECK((*swapchain)->CurrentImage().Width() == 1280);
        (*swapchain)->Present();
        (*swapchain)->Present();
    }

    TEST_CASE("a swapchain with zero images is unusable but safe") {
        GraphicsFixture fixture;
        SwapchainDesc desc;
        desc.imageCount = 0;
        auto swapchain = fixture.Device().CreateSwapchain(desc);
        REQUIRE(swapchain.HasValue());
        CHECK((*swapchain)->ImageCount() >= 2);
        bool suboptimal = false;
        CHECK((*swapchain)->AcquireNextImage(suboptimal));
    }
}
