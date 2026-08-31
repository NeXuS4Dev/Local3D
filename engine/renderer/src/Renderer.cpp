#include "local3d/renderer/Renderer.hpp"

#include "local3d/core/Assert.hpp"
#include "local3d/core/Enum.hpp"
#include "local3d/core/Log.hpp"
#include "local3d/math/Constants.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace l3d::render {
namespace {

/// Stride of the packed mesh vertex: float3 position, float3 normal, float2 uv.
constexpr u32 kMeshVertexStride = 32;
constexpr u32 kInstanceStride = static_cast<u32>(sizeof(GpuInstanceData));

[[nodiscard]] rhi::ShaderModuleDesc MakeStage(rhi::ShaderStage stage, const std::vector<u8>& code,
                                              std::string name) {
    rhi::ShaderModuleDesc desc;
    desc.stage = stage;
    desc.bytecode = std::as_bytes(std::span(code.data(), code.size()));
    desc.debugName = std::move(name);
    return desc;
}

[[nodiscard]] graph::TextureSpec TargetSpec(std::string name, u32 width, u32 height,
                                            rhi::Format format, rhi::TextureUsage usage) {
    graph::TextureSpec spec;
    spec.name = std::move(name);
    spec.width = width;
    spec.height = height;
    spec.format = format;
    spec.usage = usage;
    return spec;
}

} // namespace

std::string_view ShaderLibrary::MissingShader(bool needShadows, bool needSsao,
                                              bool needBloom) const noexcept {
    if (forwardVertex.empty()) {
        return "forwardVertex";
    }
    if (forwardFragment.empty()) {
        return "forwardFragment";
    }
    if (tonemapVertex.empty()) {
        return "tonemapVertex";
    }
    if (tonemapFragment.empty()) {
        return "tonemapFragment";
    }
    if (needShadows && (shadowVertex.empty() || shadowFragment.empty())) {
        return "shadowVertex/shadowFragment";
    }
    if (needSsao && ssaoCompute.empty()) {
        return "ssaoCompute";
    }
    if (needBloom && (bloomDownsample.empty() || bloomUpsample.empty())) {
        return "bloomDownsample/bloomUpsample";
    }
    return {};
}

Renderer::Renderer(RendererSettings settings) : settings_(std::move(settings)) {
    settings_.shadows.cascadeCount = settings_.shadows.ClampedCascadeCount();
    settings_.bloomMips = settings_.ClampedBloomMips();
}

void Renderer::SetSettings(RendererSettings settings) {
    settings_ = std::move(settings);
    settings_.shadows.cascadeCount = settings_.shadows.ClampedCascadeCount();
    settings_.bloomMips = settings_.ClampedBloomMips();
    InvalidateGraphResources();
}

void Renderer::Resize(u32 width, u32 height) {
    if (width == settings_.width && height == settings_.height) {
        return;
    }
    settings_.width = width;
    settings_.height = height;
    // Every frame sized transient is now the wrong size.
    InvalidateGraphResources();
}

void Renderer::InvalidateGraphResources() {
    graph_.Reset();
    graphResourcesValid_ = false;
    importedPresentTarget_ = nullptr;
    hdrColor_ = graph::TextureHandle{};
    sceneDepth_ = graph::TextureHandle{};
    output_ = graph::TextureHandle{};
    shadowMap_ = graph::TextureHandle{};
    ssaoTarget_ = graph::TextureHandle{};
    bloomDown_.clear();
    bloomUp_.clear();
}

void Renderer::EnsureGraphResources(rhi::ITexture* presentTarget) {
    if (graphResourcesValid_ && importedPresentTarget_ == presentTarget) {
        return;
    }
    if (!graphResourcesValid_) {
        graph_.Reset();
    } else {
        // Only the present target changed: the graph is rebuilt from scratch so
        // the imported resource does not linger.
        graph_.Reset();
    }

    const u32 width = settings_.width;
    const u32 height = settings_.height;

    hdrColor_ = graph_.CreateTexture(TargetSpec(
        "hdr-color", width, height, settings_.hdrFormat,
        rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled));
    sceneDepth_ = graph_.CreateTexture(
        TargetSpec("scene-depth", width, height, rhi::Format::Depth32_Float,
                   rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled));
    output_ = presentTarget != nullptr
                  ? graph_.ImportTexture(*presentTarget, "present-target")
                  : graph_.CreateTexture(TargetSpec(
                        "output", width, height, settings_.outputFormat,
                        rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled));
    graph_.MarkOutput(output_);

    if (settings_.enableShadows) {
        graph::TextureSpec shadowSpec =
            TargetSpec("shadow-cascades", settings_.shadows.mapSize, settings_.shadows.mapSize,
                       rhi::Format::Depth32_Float,
                       rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled);
        shadowSpec.arrayLayers = kMaxShadowCascades;
        shadowMap_ = graph_.CreateTexture(shadowSpec);
    }
    if (settings_.enableSsao) {
        ssaoTarget_ = graph_.CreateTexture(
            TargetSpec("ssao", std::max(1u, width / 2), std::max(1u, height / 2),
                       rhi::Format::R8_UNorm,
                       rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled));
    }
    if (settings_.enableBloom) {
        const u32 mips = settings_.ClampedBloomMips();
        bloomDown_.assign(mips, graph::TextureHandle{});
        bloomUp_.assign(mips, graph::TextureHandle{});
        u32 mipWidth = std::max(1u, width / 2);
        u32 mipHeight = std::max(1u, height / 2);
        for (u32 mip = 0; mip < mips; ++mip) {
            bloomDown_[mip] = graph_.CreateTexture(
                TargetSpec("bloom-down-" + std::to_string(mip), mipWidth, mipHeight,
                           rhi::Format::RGBA16_Float,
                           rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled));
            bloomUp_[mip] = graph_.CreateTexture(
                TargetSpec("bloom-up-" + std::to_string(mip), mipWidth, mipHeight,
                           rhi::Format::RGBA16_Float,
                           rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled));
            mipWidth = std::max(1u, mipWidth / 2);
            mipHeight = std::max(1u, mipHeight / 2);
        }
    }

    importedPresentTarget_ = presentTarget;
    graphResourcesValid_ = true;
}

// --- Initialisation --------------------------------------------------------

Result<void> Renderer::CreatePipelines(rhi::IDevice& device, const ShaderLibrary& shaders) {
    // Descriptor layout shared by the passes that sample the frame's resources:
    // 0 = frame uniforms, 1 = primary sampled image, 2 = secondary.
    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.debugName = "frame-set";
    rhi::DescriptorBinding uniformBinding;
    uniformBinding.binding = 0;
    uniformBinding.type = rhi::DescriptorType::UniformBuffer;
    uniformBinding.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    layoutDesc.bindings.PushBack(uniformBinding);
    rhi::DescriptorBinding primaryImage;
    primaryImage.binding = 1;
    primaryImage.type = rhi::DescriptorType::SampledImage;
    primaryImage.stages = rhi::ShaderStage::Fragment;
    layoutDesc.bindings.PushBack(primaryImage);
    rhi::DescriptorBinding secondaryImage;
    secondaryImage.binding = 2;
    secondaryImage.type = rhi::DescriptorType::SampledImage;
    secondaryImage.stages = rhi::ShaderStage::Fragment;
    layoutDesc.bindings.PushBack(secondaryImage);

    auto layout = device.CreateDescriptorSetLayout(layoutDesc);
    if (layout.IsError()) {
        return Unexpected(layout.Error());
    }
    descriptorLayout_ = std::move(*layout);
    const u32 layoutId = static_cast<u32>(descriptorLayout_->LayoutId());

    auto set = device.AllocateDescriptorSet(*descriptorLayout_);
    if (set.IsError()) {
        return Unexpected(set.Error());
    }
    frameDescriptorSet_ = std::move(*set);

    rhi::SamplerDesc samplerDesc;
    samplerDesc.debugName = "linear-clamp";
    auto sampler = device.CreateSampler(samplerDesc);
    if (sampler.IsError()) {
        return Unexpected(sampler.Error());
    }
    linearSampler_ = std::move(*sampler);

    // Mesh vertex layout: float3 position, float3 normal, float2 uv.
    rhi::VertexBinding meshBinding;
    meshBinding.binding = 0;
    meshBinding.stride = kMeshVertexStride;
    rhi::VertexBinding instanceBinding;
    instanceBinding.binding = 1;
    instanceBinding.stride = kInstanceStride;
    instanceBinding.perInstance = true;

    std::array<rhi::VertexAttribute, 8> attributes{};
    attributes[0] = rhi::VertexAttribute{0, 0, rhi::Format::RGB32_Float, 0};
    attributes[1] = rhi::VertexAttribute{1, 0, rhi::Format::RGB32_Float, 12};
    attributes[2] = rhi::VertexAttribute{2, 0, rhi::Format::RG32_Float, 24};
    // Instance: four rows of the world matrix, then the tint.
    for (u32 row = 0; row < 4; ++row) {
        attributes[3 + row] =
            rhi::VertexAttribute{3 + row, 1, rhi::Format::RGBA32_Float, row * 16};
    }
    attributes[7] = rhi::VertexAttribute{7, 1, rhi::Format::RGBA32_Float, 64};

    // --- Forward lit pass -------------------------------------------------
    rhi::PipelineDesc forwardDesc;
    forwardDesc.shaders.PushBack(
        MakeStage(rhi::ShaderStage::Vertex, shaders.forwardVertex, "forward.vs"));
    forwardDesc.shaders.PushBack(
        MakeStage(rhi::ShaderStage::Fragment, shaders.forwardFragment, "forward.fs"));
    forwardDesc.vertexBindings.PushBack(meshBinding);
    forwardDesc.vertexBindings.PushBack(instanceBinding);
    for (const rhi::VertexAttribute& attribute : attributes) {
        forwardDesc.vertexAttributes.PushBack(attribute);
    }
    forwardDesc.colorFormats.PushBack(settings_.hdrFormat);
    forwardDesc.depthFormat = rhi::Format::Depth32_Float;
    forwardDesc.hasDepthAttachment = true;
    forwardDesc.descriptorSetLayoutIds.PushBack(layoutId);
    auto forward = device.CreateGraphicsPipeline(forwardDesc);
    if (forward.IsError()) {
        return Unexpected(forward.Error());
    }
    forwardPipeline_ = std::move(*forward);

    // --- Shadow pass: depth only -----------------------------------------
    if (settings_.enableShadows) {
        rhi::PipelineDesc shadowDesc;
        shadowDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Vertex, shaders.shadowVertex, "shadow.vs"));
        shadowDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Fragment, shaders.shadowFragment, "shadow.fs"));
        shadowDesc.vertexBindings.PushBack(meshBinding);
        shadowDesc.vertexBindings.PushBack(instanceBinding);
        for (const rhi::VertexAttribute& attribute : attributes) {
            shadowDesc.vertexAttributes.PushBack(attribute);
        }
        shadowDesc.depthFormat = rhi::Format::Depth32_Float;
        shadowDesc.hasDepthAttachment = true;
        shadowDesc.debugName = "shadow-csm";
        auto shadow = device.CreateGraphicsPipeline(shadowDesc);
        if (shadow.IsError()) {
            return Unexpected(shadow.Error());
        }
        shadowPipeline_ = std::move(*shadow);
    }

    // --- Tonemap ----------------------------------------------------------
    rhi::PipelineDesc tonemapDesc;
    tonemapDesc.shaders.PushBack(
        MakeStage(rhi::ShaderStage::Vertex, shaders.tonemapVertex, "tonemap.vs"));
    tonemapDesc.shaders.PushBack(
        MakeStage(rhi::ShaderStage::Fragment, shaders.tonemapFragment, "tonemap.fs"));
    tonemapDesc.colorFormats.PushBack(settings_.outputFormat);
    tonemapDesc.descriptorSetLayoutIds.PushBack(layoutId);
    tonemapDesc.debugName = "tonemap";
    auto tonemap = device.CreateGraphicsPipeline(tonemapDesc);
    if (tonemap.IsError()) {
        return Unexpected(tonemap.Error());
    }
    tonemapPipeline_ = std::move(*tonemap);

    // --- Compute passes ---------------------------------------------------
    rhi::PushConstantRange pushRange;
    pushRange.stage = rhi::ShaderStage::Compute;
    pushRange.size = 16;

    if (settings_.enableBloom) {
        rhi::PipelineDesc downDesc;
        downDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Compute, shaders.bloomDownsample, "bloom-down.cs"));
        downDesc.pushConstantRanges.PushBack(pushRange);
        auto down = device.CreateComputePipeline(downDesc);
        if (down.IsError()) {
            return Unexpected(down.Error());
        }
        bloomDownPipeline_ = std::move(*down);

        rhi::PipelineDesc upDesc;
        upDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Compute, shaders.bloomUpsample, "bloom-up.cs"));
        upDesc.pushConstantRanges.PushBack(pushRange);
        auto up = device.CreateComputePipeline(upDesc);
        if (up.IsError()) {
            return Unexpected(up.Error());
        }
        bloomUpPipeline_ = std::move(*up);
    }

    if (settings_.enableSsao) {
        rhi::PipelineDesc prepassDesc;
        prepassDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Vertex, shaders.shadowVertex, "prepass.vs"));
        prepassDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Fragment, shaders.shadowFragment, "prepass.fs"));
        prepassDesc.vertexBindings.PushBack(meshBinding);
        prepassDesc.vertexBindings.PushBack(instanceBinding);
        for (const rhi::VertexAttribute& attribute : attributes) {
            prepassDesc.vertexAttributes.PushBack(attribute);
        }
        prepassDesc.depthFormat = rhi::Format::Depth32_Float;
        prepassDesc.hasDepthAttachment = true;
        prepassDesc.debugName = "depth-prepass";
        auto prepass = device.CreateGraphicsPipeline(prepassDesc);
        if (prepass.IsError()) {
            return Unexpected(prepass.Error());
        }
        prepassPipeline_ = std::move(*prepass);

        rhi::PipelineDesc ssaoDesc;
        ssaoDesc.shaders.PushBack(
            MakeStage(rhi::ShaderStage::Compute, shaders.ssaoCompute, "ssao.cs"));
        ssaoDesc.pushConstantRanges.PushBack(pushRange);
        auto ssao = device.CreateComputePipeline(ssaoDesc);
        if (ssao.IsError()) {
            return Unexpected(ssao.Error());
        }
        ssaoPipeline_ = std::move(*ssao);
    }

    // --- Render passes ----------------------------------------------------
    rhi::RenderPassDesc forwardPass;
    rhi::AttachmentDesc hdrAttachment;
    hdrAttachment.format = settings_.hdrFormat;
    forwardPass.colorAttachments.PushBack(hdrAttachment);
    forwardPass.hasDepthStencil = true;
    forwardPass.depthStencil.format = rhi::Format::Depth32_Float;
    // With a prepass the depth buffer arrives filled and is only tested against.
    forwardPass.depthStencil.loadOp =
        settings_.enableSsao ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
    forwardPass.debugName = "forward-opaque";
    auto forwardRenderPass = device.CreateRenderPass(forwardPass);
    if (forwardRenderPass.IsError()) {
        return Unexpected(forwardRenderPass.Error());
    }
    forwardRenderPass_ = std::move(*forwardRenderPass);

    rhi::RenderPassDesc shadowPass;
    shadowPass.hasDepthStencil = true;
    shadowPass.depthStencil.format = rhi::Format::Depth32_Float;
    shadowPass.debugName = "shadow";
    auto shadowRenderPass = device.CreateRenderPass(shadowPass);
    if (shadowRenderPass.IsError()) {
        return Unexpected(shadowRenderPass.Error());
    }
    shadowRenderPass_ = std::move(*shadowRenderPass);

    rhi::RenderPassDesc tonemapPass;
    rhi::AttachmentDesc outputAttachment;
    outputAttachment.format = settings_.outputFormat;
    tonemapPass.colorAttachments.PushBack(outputAttachment);
    tonemapPass.debugName = "tonemap";
    auto tonemapRenderPass = device.CreateRenderPass(tonemapPass);
    if (tonemapRenderPass.IsError()) {
        return Unexpected(tonemapRenderPass.Error());
    }
    tonemapRenderPass_ = std::move(*tonemapRenderPass);

    // --- Persistent buffers ----------------------------------------------
    rhi::BufferDesc uniformDesc;
    uniformDesc.size = sizeof(FrameUniforms);
    uniformDesc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDestination;
    uniformDesc.memory = rhi::MemoryType::CpuToGpu;
    uniformDesc.debugName = "frame-uniforms";
    auto uniformBuffer = device.CreateBuffer(uniformDesc);
    if (uniformBuffer.IsError()) {
        return Unexpected(uniformBuffer.Error());
    }
    uniformBuffer_ = std::move(*uniformBuffer);

    return {};
}

Result<void> Renderer::Initialize(rhi::IDevice& device, const ShaderLibrary& shaders) {
    const std::string_view missing = shaders.MissingShader(settings_.enableShadows,
                                                           settings_.enableSsao,
                                                           settings_.enableBloom);
    if (!missing.empty()) {
        return Unexpected(Status{StatusCode::InvalidArgument,
                                 std::string("Renderer is missing shader '") +
                                     std::string(missing) + "'"});
    }
    if (settings_.width == 0 || settings_.height == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Renderer size must be non-zero"});
    }
    if (auto result = CreatePipelines(device, shaders); result.IsError()) {
        return result;
    }
    initialized_ = true;
    return {};
}

// --- Geometry --------------------------------------------------------------

Result<MeshHandle> Renderer::RegisterMesh(rhi::IDevice& device, const MeshData& data) {
    if (!data.IsValid()) {
        return Unexpected(
            Status{StatusCode::InvalidArgument, "Mesh '" + data.name + "' is incomplete"});
    }

    // Pack position/normal/uv into the 32 byte vertex the pipeline declares.
    std::vector<u8> packed(data.positions.size() * kMeshVertexStride);
    for (usize i = 0; i < data.positions.size(); ++i) {
        u8* dst = packed.data() + i * kMeshVertexStride;
        std::memcpy(dst, &data.positions[i], sizeof(math::Vec3));
        std::memcpy(dst + 12, &data.normals[i], sizeof(math::Vec3));
        std::memcpy(dst + 24, &data.uvs[i], sizeof(math::Vec2));
    }

    rhi::BufferDesc vertexDesc;
    vertexDesc.size = packed.size();
    vertexDesc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDestination;
    vertexDesc.debugName = data.name + ".vertices";
    auto vertices = device.CreateBuffer(vertexDesc);
    if (vertices.IsError()) {
        return Unexpected(vertices.Error());
    }
    device.UpdateBuffer(**vertices, 0, std::as_bytes(std::span(packed.data(), packed.size())));

    rhi::BufferDesc indexDesc;
    indexDesc.size = data.indices.size() * sizeof(u32);
    indexDesc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDestination;
    indexDesc.debugName = data.name + ".indices";
    auto indices = device.CreateBuffer(indexDesc);
    if (indices.IsError()) {
        return Unexpected(indices.Error());
    }
    device.UpdateBuffer(**indices, 0, std::as_bytes(std::span(data.indices.data(),
                                                             data.indices.size())));

    GpuMesh mesh;
    mesh.vertices = std::move(*vertices);
    mesh.indices = std::move(*indices);
    mesh.vertexCount = static_cast<u32>(data.positions.size());
    mesh.indexCount = static_cast<u32>(data.indices.size());
    mesh.bounds = data.bounds;
    meshes_.push_back(std::move(mesh));
    return static_cast<MeshHandle>(meshes_.size() - 1);
}

// --- Frame preparation -----------------------------------------------------

void Renderer::ComputeCascades(const FrameView& view, const DirectionalLight& sun) {
    cascades_.clear();
    if (!settings_.enableShadows || !sun.castsShadow) {
        return;
    }
    const auto splits = ComputeCascadeSplits(view.nearPlane, view.farPlane, settings_.shadows);
    f32 previous = view.nearPlane;
    for (const f32 split : splits) {
        cascades_.push_back(
            ComputeCascade(view, sun.direction, previous, split, settings_.shadows));
        previous = split;
    }
    for (usize i = 0; i < cascades_.size() && i < kMaxShadowCascades; ++i) {
        uniforms_.cascadeViewProjections[i] = cascades_[i].viewProjection;
    }
    uniforms_.cascadeSplits = math::Vec4{
        cascades_.empty() ? 0.0f : cascades_[0].splitViewDepth,
        cascades_.size() < 2 ? 0.0f : cascades_[1].splitViewDepth,
        cascades_.size() < 3 ? 0.0f : cascades_[2].splitViewDepth,
        cascades_.size() < 4 ? 0.0f : cascades_[3].splitViewDepth};
}

void Renderer::BuildBatches(std::span<const DrawItem> items) {
    instances_.clear();
    batches_.clear();
    shadowBatches_.clear();
    if (cullResult_.visible.empty()) {
        return;
    }

    // Group by mesh so each unique mesh is drawn once per frame.  `visible` is in
    // input order, so the grouping is stable frame to frame.
    for (usize v = 0; v < cullResult_.visible.size(); ++v) {
        const DrawItem& item = items[cullResult_.visible[v]];
        const u8 lod = v < cullResult_.lodIndex.size() ? cullResult_.lodIndex[v] : 0;
        const MeshHandle mesh = item.lods[lod < item.lods.size() ? lod : 0];
        std::fprintf(stderr, "[dbg] v=%zu visible=%u lod=%u mesh=%u meshes=%zu lods0=%u\n", v,
                     cullResult_.visible[v], static_cast<unsigned>(lod), mesh, meshes_.size(),
                     item.lods[0]);
        if (mesh == kInvalidMesh || mesh >= meshes_.size()) {
            continue;
        }
        GpuInstanceData instance;
        instance.world = item.world;
        instance.tint = item.tint;

        if (!batches_.empty() && batches_.back().mesh == mesh) {
            ++batches_.back().instanceCount;
        } else {
            batches_.push_back(Batch{mesh, static_cast<u32>(instances_.size()), 1});
        }
        if (item.castsShadow && settings_.enableShadows) {
            if (!shadowBatches_.empty() && shadowBatches_.back().mesh == mesh) {
                ++shadowBatches_.back().instanceCount;
            } else {
                shadowBatches_.push_back(Batch{mesh, static_cast<u32>(instances_.size()), 1});
            }
        }
        instances_.push_back(instance);
    }
}

Result<void> Renderer::UploadInstances(rhi::IDevice& device) {
    if (instances_.empty()) {
        return {};
    }
    const u64 bytes = instances_.size() * sizeof(GpuInstanceData);
    if (!instanceBuffer_ || instanceBuffer_->Size() < bytes) {
        // Grow only: resizing down every frame would churn allocations.
        u64 capacity = 64 * sizeof(GpuInstanceData);
        while (capacity < bytes) {
            capacity *= 2;
        }
        rhi::BufferDesc desc;
        desc.size = capacity;
        desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDestination;
        desc.debugName = "instance-data";
        auto buffer = device.CreateBuffer(desc);
        if (buffer.IsError()) {
            return Unexpected(buffer.Error());
        }
        instanceBuffer_ = std::move(*buffer);
    }
    const auto* data = reinterpret_cast<const std::byte*>(instances_.data()); // NOLINT
    device.UpdateBuffer(*instanceBuffer_, 0, std::span<const std::byte>(data, bytes));
    return {};
}

Result<rhi::IFramebuffer*> Renderer::AcquireFramebuffer(
    rhi::IDevice& device, FramebufferCache& cache, const rhi::IRenderPass& pass,
    std::span<const rhi::ITexture* const> attachments, u32 width, u32 height, std::string name) {
    const bool sameAttachments =
        cache.attachments.size() == attachments.size() &&
        std::equal(cache.attachments.begin(), cache.attachments.end(), attachments.begin());
    if (cache.framebuffer && sameAttachments && cache.width == width && cache.height == height) {
        return cache.framebuffer.get();
    }
    auto framebuffer =
        device.CreateFramebuffer(pass, attachments, width, height, std::move(name));
    if (framebuffer.IsError()) {
        return Unexpected(framebuffer.Error());
    }
    cache.framebuffer = std::move(*framebuffer);
    cache.attachments.assign(attachments.begin(), attachments.end());
    cache.width = width;
    cache.height = height;
    return cache.framebuffer.get();
}

void Renderer::RecordBatches(graph::PassContext& context, const std::vector<Batch>& batches) {
    if (instanceBuffer_ == nullptr) {
        return;
    }
    for (const Batch& batch : batches) {
        if (batch.mesh >= meshes_.size()) {
            continue;
        }
        const GpuMesh& mesh = meshes_[batch.mesh];
        context.Commands().BindVertexBuffer(0, *mesh.vertices);
        context.Commands().BindVertexBuffer(1, *instanceBuffer_);
        context.Commands().BindIndexBuffer(*mesh.indices, 0, true);
        context.Commands().DrawIndexed(mesh.indexCount, batch.instanceCount, 0, 0,
                                       batch.firstInstance);
    }
}

void Renderer::UploadUniforms(rhi::IDevice& device) {
    const std::byte* bytes = reinterpret_cast<const std::byte*>(&uniforms_); // NOLINT
    device.UpdateBuffer(*uniformBuffer_, 0,
                        std::span<const std::byte>(bytes, sizeof(FrameUniforms)));
}

// --- Graph -----------------------------------------------------------------

void Renderer::BuildGraph(const DirectionalLight& sun, rhi::ITexture* presentTarget) {
    EnsureGraphResources(presentTarget);
    graph_.ClearPasses();

    const graph::TextureHandle hdrColor = hdrColor_;
    const graph::TextureHandle sceneDepth = sceneDepth_;
    const graph::TextureHandle output = output_;
    const graph::TextureHandle ssaoTarget = ssaoTarget_;

    graph::TextureHandle shadowMap;
    if (settings_.enableShadows && sun.castsShadow && !cascades_.empty()) {
        shadowMap = shadowMap_;
        // Ordering comes from the forward pass reading this map, so the shadow
        // pass itself declares no inputs.
        graph_.AddRasterPass(
            "shadow-csm", [shadowMap](graph::PassBuilder& builder) { builder.Write(shadowMap); },
            [this, shadowMap](graph::PassContext& context) {
                RecordShadowPass(context, shadowMap);
            });
    }

    if (settings_.enableSsao) {
        graph_.AddRasterPass(
            "depth-prepass", [sceneDepth](graph::PassBuilder& builder) {
                builder.Write(sceneDepth);
            },
            [this, sceneDepth](graph::PassContext& context) {
                RecordDepthPrepass(context, sceneDepth);
            });
    }

    graph_.AddRasterPass(
        "forward-opaque",
        [&](graph::PassBuilder& builder) {
            builder.Write(hdrColor);
            if (ssaoTarget.IsValid()) {
                // Depth came from the prepass, so the forward pass only reads it.
                builder.Read(sceneDepth);
                builder.Read(ssaoTarget);
            } else {
                builder.Write(sceneDepth);
            }
            if (shadowMap.IsValid()) {
                builder.Read(shadowMap);
            }
        },
        [this, hdrColor, sceneDepth](graph::PassContext& context) {
            RecordForwardPass(context, hdrColor, sceneDepth);
        });

    if (settings_.enableSsao) {
        const u32 tilesX = (std::max(1u, settings_.width / 2) + 7) / 8;
        const u32 tilesY = (std::max(1u, settings_.height / 2) + 7) / 8;
        graph_.AddComputePass(
            "ssao",
            [&](graph::PassBuilder& builder) {
                builder.Read(sceneDepth);
                builder.Write(ssaoTarget);
            },
            [this, tilesX, tilesY](graph::PassContext& context) {
                context.Commands().BindPipeline(*ssaoPipeline_);
                context.Commands().Dispatch(tilesX, tilesY, 1);
            });
    }

    if (settings_.enableBloom) {
        const u32 mips = settings_.ClampedBloomMips();
        for (u32 mip = 0; mip < mips; ++mip) {
            const graph::TextureHandle source = mip == 0 ? hdrColor : bloomDown_[mip - 1];
            const graph::TextureHandle target = bloomDown_[mip];
            graph_.AddComputePass(
                "bloom-down-" + std::to_string(mip),
                [source, target](graph::PassBuilder& builder) {
                    builder.Read(source);
                    builder.Write(target);
                },
                [this](graph::PassContext& context) {
                    context.Commands().BindPipeline(*bloomDownPipeline_);
                    context.Commands().Dispatch(1, 1, 1);
                });
        }
        for (u32 mip = mips; mip-- > 0;) {
            const graph::TextureHandle source = bloomDown_[mip];
            const graph::TextureHandle coarser =
                mip + 1 < mips ? bloomUp_[mip + 1] : graph::TextureHandle{};
            const graph::TextureHandle target = bloomUp_[mip];
            graph_.AddComputePass(
                "bloom-up-" + std::to_string(mip),
                [source, coarser, target](graph::PassBuilder& builder) {
                    builder.Read(source);
                    if (coarser.IsValid()) {
                        builder.Read(coarser);
                    }
                    builder.Write(target);
                },
                [this](graph::PassContext& context) {
                    context.Commands().BindPipeline(*bloomUpPipeline_);
                    context.Commands().Dispatch(1, 1, 1);
                });
        }

        const graph::TextureHandle bloom = bloomUp_[0];
        graph_.AddRasterPass(
            "tonemap",
            [&, bloom](graph::PassBuilder& builder) {
                builder.Read(hdrColor);
                builder.Read(bloom);
                builder.Write(output);
            },
            [this, output](graph::PassContext& context) {
                RecordTonemapPass(context, output);
            });
    } else {
        graph_.AddRasterPass(
            "tonemap",
            [&](graph::PassBuilder& builder) {
                builder.Read(hdrColor);
                builder.Write(output);
            },
            [this, output](graph::PassContext& context) {
                RecordTonemapPass(context, output);
            });
    }
}

// --- Recording -------------------------------------------------------------

void Renderer::RecordShadowPass(graph::PassContext& context, graph::TextureHandle shadowMap) {
    rhi::ITexture* map = context.Texture(shadowMap);
    if (map == nullptr || shadowBatches_.empty()) {
        return;
    }
    const std::array<const rhi::ITexture* const, 1> attachments{map};
    auto framebuffer = AcquireFramebuffer(context.Device(), shadowFramebuffer_, *shadowRenderPass_,
                                          attachments, map->Width(), map->Height(),
                                          "shadow-target");
    if (framebuffer.IsError()) {
        return;
    }
    std::array<rhi::ClearValue, 1> clearValues{};
    clearValues[0].clearColor = false;
    clearValues[0].depth = 1.0f;

    // One pass per cascade; the null backend renders into the array texture as a
    // whole, a real backend selects the layer through the framebuffer view.
    for (usize cascade = 0; cascade < cascades_.size(); ++cascade) {
        context.Commands().BeginRenderPass(*shadowRenderPass_, **framebuffer, clearValues);
        context.Commands().BindPipeline(*shadowPipeline_);
        rhi::Viewport viewport;
        viewport.width = static_cast<f32>(map->Width());
        viewport.height = static_cast<f32>(map->Height());
        context.Commands().SetViewport(viewport);
        RecordBatches(context, shadowBatches_);
        context.Commands().EndRenderPass();
    }
}

void Renderer::RecordDepthPrepass(graph::PassContext& context, graph::TextureHandle depth) {
    rhi::ITexture* depthTarget = context.Texture(depth);
    if (depthTarget == nullptr || batches_.empty()) {
        return;
    }
    const std::array<const rhi::ITexture* const, 1> attachments{depthTarget};
    auto framebuffer = AcquireFramebuffer(context.Device(), prepassFramebuffer_,
                                          *shadowRenderPass_, attachments, depthTarget->Width(),
                                          depthTarget->Height(), "prepass-target");
    if (framebuffer.IsError()) {
        return;
    }
    std::array<rhi::ClearValue, 1> clearValues{};
    clearValues[0].clearColor = false;
    clearValues[0].depth = 1.0f;

    context.Commands().BeginRenderPass(*shadowRenderPass_, **framebuffer, clearValues);
    context.Commands().BindPipeline(*prepassPipeline_);
    context.Commands().SetViewport(lastView_.Viewport());
    RecordBatches(context, batches_);
    context.Commands().EndRenderPass();
}

void Renderer::RecordForwardPass(graph::PassContext& context, graph::TextureHandle color,
                                 graph::TextureHandle depth) {
    rhi::ITexture* colorTarget = context.Texture(color);
    rhi::ITexture* depthTarget = context.Texture(depth);
    if (colorTarget == nullptr || depthTarget == nullptr) {
        return;
    }
    const std::array<const rhi::ITexture* const, 2> attachments{colorTarget, depthTarget};
    auto framebuffer = AcquireFramebuffer(context.Device(), forwardFramebuffer_,
                                          *forwardRenderPass_, attachments, colorTarget->Width(),
                                          colorTarget->Height(), "hdr-target");
    if (framebuffer.IsError()) {
        return;
    }

    // Rewrite the frame descriptor set with this frame's resources.
    std::vector<rhi::DescriptorWrite> writes;
    rhi::DescriptorWrite uniformWrite;
    uniformWrite.binding = 0;
    uniformWrite.type = rhi::DescriptorType::UniformBuffer;
    uniformWrite.buffer = uniformBuffer_.get();
    uniformWrite.bufferRange = sizeof(FrameUniforms);
    writes.push_back(uniformWrite);
    rhi::DescriptorWrite depthWrite;
    depthWrite.binding = 1;
    depthWrite.type = rhi::DescriptorType::SampledImage;
    depthWrite.texture = depthTarget;
    writes.push_back(depthWrite);
    rhi::DescriptorWrite colorWrite;
    colorWrite.binding = 2;
    colorWrite.type = rhi::DescriptorType::SampledImage;
    colorWrite.texture = colorTarget;
    writes.push_back(colorWrite);
    context.Device().WriteDescriptorSet(*frameDescriptorSet_, writes);

    std::array<rhi::ClearValue, 1> clearValues{};
    clearValues[0].color = settings_.clearColor;
    clearValues[0].depth = 1.0f;

    context.Commands().BeginRenderPass(*forwardRenderPass_, **framebuffer, clearValues);
    context.Commands().BindPipeline(*forwardPipeline_);
    context.Commands().SetViewport(lastView_.Viewport());
    context.Commands().BindDescriptorSet(0, *descriptorLayout_, *frameDescriptorSet_);
    RecordBatches(context, batches_);
    context.Commands().EndRenderPass();
}

void Renderer::RecordTonemapPass(graph::PassContext& context, graph::TextureHandle output) {
    rhi::ITexture* target = context.Texture(output);
    if (target == nullptr) {
        return;
    }
    const std::array<const rhi::ITexture* const, 1> attachments{target};
    auto framebuffer = AcquireFramebuffer(context.Device(), tonemapFramebuffer_,
                                          *tonemapRenderPass_, attachments, target->Width(),
                                          target->Height(), "output-target");
    if (framebuffer.IsError()) {
        return;
    }
    std::vector<rhi::DescriptorWrite> writes;
    rhi::DescriptorWrite uniformWrite;
    uniformWrite.binding = 0;
    uniformWrite.type = rhi::DescriptorType::UniformBuffer;
    uniformWrite.buffer = uniformBuffer_.get();
    uniformWrite.bufferRange = sizeof(FrameUniforms);
    writes.push_back(uniformWrite);
    context.Device().WriteDescriptorSet(*frameDescriptorSet_, writes);

    std::array<rhi::ClearValue, 1> clearValues{};
    clearValues[0].clearDepth = false;
    context.Commands().BeginRenderPass(*tonemapRenderPass_, **framebuffer, clearValues);
    context.Commands().BindPipeline(*tonemapPipeline_);
    rhi::Viewport viewport;
    viewport.width = static_cast<f32>(target->Width());
    viewport.height = static_cast<f32>(target->Height());
    context.Commands().SetViewport(viewport);
    context.Commands().BindDescriptorSet(0, *descriptorLayout_, *frameDescriptorSet_);
    // Fullscreen triangle; the vertex shader derives positions from the id.
    context.Commands().Draw(3);
    context.Commands().EndRenderPass();
}

Result<void> Renderer::RenderFrame(rhi::IDevice& device, rhi::ICommandBuffer& commands,
                                   const FrameView& view, std::span<const DrawItem> items,
                                   const DirectionalLight& sun, const IblParameters& ibl,
                                   rhi::ITexture* presentTarget) {
    if (!initialized_) {
        return Unexpected(
            Status{StatusCode::InvalidState, "Renderer::RenderFrame before Initialize"});
    }
    L3D_UNUSED(ibl);

    lastView_ = view;
    stats_ = RendererStats{};
    stats_.drawItems = static_cast<u32>(items.size());

    CullDrawItems(view, items, cullResult_);
    stats_.visibleItems = static_cast<u32>(cullResult_.visible.size());
    stats_.frustumCulled = cullResult_.frustumCulled;
    stats_.lodHistogram = cullResult_.lodHistogram;

    uniforms_ = FrameUniforms{};
    uniforms_.view = view.view;
    uniforms_.projection = view.projection;
    uniforms_.viewProjection = view.viewProjection;
    uniforms_.inverseViewProjection = view.viewProjection.Inverse();
    uniforms_.cameraPositionAndExposure =
        math::Vec4{view.position.x, view.position.y, view.position.z, settings_.exposure};
    const math::Vec3 sunDir = math::Normalize(sun.direction);
    uniforms_.sunDirectionAndIntensity =
        math::Vec4{sunDir.x, sunDir.y, sunDir.z, sun.castsShadow ? sun.intensity : 0.0f};
    uniforms_.sunColor = math::Vec4{sun.color.x, sun.color.y, sun.color.z, 1.0f};
    uniforms_.renderTargetSize = math::Vec4{
        static_cast<f32>(settings_.width), static_cast<f32>(settings_.height),
        1.0f / static_cast<f32>(settings_.width), 1.0f / static_cast<f32>(settings_.height)};

    ComputeCascades(view, sun);

    f32 flags = static_cast<f32>(cascades_.size());
    flags += (settings_.enableSsao ? 16.0f : 0.0f);
    flags += (settings_.enableBloom ? 32.0f : 0.0f);
    flags += (settings_.enableTaa ? 64.0f : 0.0f);
    uniforms_.flagsAndParams = math::Vec4{flags, settings_.exposure, 0.0f, 0.0f};

    BuildBatches(items);
    stats_.batches = static_cast<u32>(batches_.size());
    stats_.instances = static_cast<u32>(instances_.size());
    stats_.shadowCasters = 0;
    for (const Batch& batch : shadowBatches_) {
        stats_.shadowCasters += batch.instanceCount;
    }

    if (auto uploaded = UploadInstances(device); uploaded.IsError()) {
        return uploaded;
    }
    UploadUniforms(device);

    BuildGraph(sun, presentTarget);
    if (auto compiled = graph_.Compile(); compiled.IsError()) {
        return compiled;
    }
    stats_.graphPasses = graph_.Stats().executedPasses;
    stats_.graphCulledPasses = graph_.Stats().culledPasses;
    stats_.transientBytes = graph_.Stats().transientBytes;

    const auto before = commands.GetStats();
    if (auto executed = graph_.Execute(device, commands); executed.IsError()) {
        return executed;
    }
    const auto after = commands.GetStats();
    stats_.drawCalls = (after.drawCalls - before.drawCalls) +
                       (after.indexedDrawCalls - before.indexedDrawCalls);
    stats_.dispatches = after.dispatches - before.dispatches;
    return {};
}

} // namespace l3d::render
