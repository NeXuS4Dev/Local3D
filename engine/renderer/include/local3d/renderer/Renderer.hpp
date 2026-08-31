#pragma once
/// @file Renderer.hpp
/// @brief Builds and executes the frame: cull, batch, upload, record.
///
/// The renderer owns no scene.  It is given a view and a list of draw items each
/// frame and produces recorded commands through the render graph.  That keeps the
/// scene system, the editor viewport and headless tests interchangeable.

#include "local3d/core/Result.hpp"
#include "local3d/math/Color.hpp"
#include "local3d/renderer/FrameView.hpp"
#include "local3d/renderer/RenderTypes.hpp"
#include "local3d/rendergraph/RenderGraph.hpp"
#include "local3d/rhi/RhiDevice.hpp"

#include <span>
#include <string>
#include <vector>

namespace l3d::render {

/// Bytecode for every pass the renderer can build.  Produced by the shader
/// pipeline; the renderer never invents shader code.  A missing blob is an
/// initialise-time error rather than a silently skipped feature.
struct ShaderLibrary {
    std::vector<u8> forwardVertex;
    std::vector<u8> forwardFragment;
    std::vector<u8> shadowVertex;
    std::vector<u8> shadowFragment;
    std::vector<u8> bloomDownsample; ///< compute
    std::vector<u8> bloomUpsample;   ///< compute
    std::vector<u8> ssaoCompute;     ///< compute
    std::vector<u8> tonemapVertex;
    std::vector<u8> tonemapFragment;

    /// First missing shader, or empty when the library is complete enough for
    /// the requested settings.
    [[nodiscard]] std::string_view MissingShader(bool needShadows, bool needSsao,
                                                 bool needBloom) const noexcept;
};

struct RendererSettings {
    u32 width = 1280;
    u32 height = 720;
    rhi::Format hdrFormat = rhi::Format::RGBA16_Float;
    rhi::Format outputFormat = rhi::Format::BGRA8_UNorm;
    bool enableShadows = true;
    ShadowSettings shadows;
    bool enableSsao = true;
    bool enableBloom = true;
    /// Bloom mip chain length.  Each mip is a quarter of the previous one.
    u32 bloomMips = 5;
    bool enableTaa = true;
    f32 exposure = 1.0f;
    math::Color clearColor{0.05f, 0.06f, 0.09f, 1.0f};

    [[nodiscard]] u32 ClampedBloomMips() const noexcept {
        return bloomMips == 0 ? 1 : (bloomMips > 8 ? 8 : bloomMips);
    }
};

/// What the GPU sees once per frame.  Layout must match the shader's uniform
/// block; it is uploaded whole with a single UpdateBuffer.
struct FrameUniforms {
    math::Mat4 view;
    math::Mat4 projection;
    math::Mat4 viewProjection;
    math::Mat4 inverseViewProjection;
    std::array<math::Mat4, kMaxShadowCascades> cascadeViewProjections{};
    math::Vec4 cameraPositionAndExposure{};
    math::Vec4 sunDirectionAndIntensity{};
    math::Vec4 sunColor{};
    math::Vec4 cascadeSplits{}; ///< View space split depth per cascade.
    math::Vec4 renderTargetSize{}; ///< xy = size, zw = 1/size
    math::Vec4 flagsAndParams{}; ///< x = cascade count, y = ao/bloom/taa flags, z = exposure
};

class Renderer {
public:
    explicit Renderer(RendererSettings settings);

    /// Creates the pipelines, uniform buffer and descriptor layout.  Fails when a
    /// shader needed by the enabled features is missing.
    [[nodiscard]] Result<void> Initialize(rhi::IDevice& device, const ShaderLibrary& shaders);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] const RendererSettings& Settings() const noexcept { return settings_; }
    /// Changing settings rebuilds the graph on the next frame.
    void SetSettings(RendererSettings settings);
    void Resize(u32 width, u32 height);

    /// Uploads geometry and returns a handle usable from DrawItem::lods.
    [[nodiscard]] Result<MeshHandle> RegisterMesh(rhi::IDevice& device, const MeshData& data);
    [[nodiscard]] usize MeshCount() const noexcept { return meshes_.size(); }

    /// Cull, batch, upload uniforms, build the frame graph and execute it.
    /// `presentTarget` may be null, in which case the output is a transient.
    [[nodiscard]] Result<void> RenderFrame(rhi::IDevice& device, rhi::ICommandBuffer& commands,
                                           const FrameView& view, std::span<const DrawItem> items,
                                           const DirectionalLight& sun, const IblParameters& ibl,
                                           rhi::ITexture* presentTarget = nullptr);

    [[nodiscard]] const RendererStats& Stats() const noexcept { return stats_; }
    [[nodiscard]] const graph::RenderGraph& Graph() const noexcept { return graph_; }
    [[nodiscard]] const std::vector<ShadowCascade>& Cascades() const noexcept { return cascades_; }
    [[nodiscard]] const FrameUniforms& Uniforms() const noexcept { return uniforms_; }
    /// The instance data uploaded for the last frame, for tests and debugging.
    [[nodiscard]] const std::vector<GpuInstanceData>& LastInstances() const noexcept {
        return instances_;
    }
    [[nodiscard]] const CullResult& LastCullResult() const noexcept { return cullResult_; }
    /// GPU buffers for the last frame.  Exposed so tests and the frame debugger
    /// can read back exactly what was uploaded (Map() on a CpuToGpu buffer).
    [[nodiscard]] rhi::IBuffer& UniformBuffer() { return *uniformBuffer_; }
    [[nodiscard]] const rhi::IBuffer* InstanceBuffer() const noexcept {
        return instanceBuffer_.get();
    }

private:
    struct GpuMesh {
        rhi::BufferPtr vertices;
        rhi::BufferPtr indices;
        u32 vertexCount = 0;
        u32 indexCount = 0;
        math::Aabb bounds;
    };
    struct Batch {
        MeshHandle mesh = kInvalidMesh;
        u32 firstInstance = 0;
        u32 instanceCount = 0;
    };

    /// A framebuffer that is rebuilt only when its attachments or size change.
    struct FramebufferCache {
        rhi::FramebufferPtr framebuffer;
        std::vector<const rhi::ITexture*> attachments;
        u32 width = 0;
        u32 height = 0;
    };

    [[nodiscard]] Result<void> CreatePipelines(rhi::IDevice& device, const ShaderLibrary& shaders);
    /// Creates the frame's graph resources once; later frames only rebuild the
    /// pass list.  Re-creating them every frame would grow the graph without end.
    void EnsureGraphResources(rhi::ITexture* presentTarget);
    void InvalidateGraphResources();
    void BuildGraph(const DirectionalLight& sun, rhi::ITexture* presentTarget);
    void RecordForwardPass(graph::PassContext& context, graph::TextureHandle color,
                           graph::TextureHandle depth);
    void RecordDepthPrepass(graph::PassContext& context, graph::TextureHandle depth);
    void RecordShadowPass(graph::PassContext& context, graph::TextureHandle shadowMap);
    void RecordTonemapPass(graph::PassContext& context, graph::TextureHandle output);
    [[nodiscard]] Result<rhi::IFramebuffer*> AcquireFramebuffer(rhi::IDevice& device,
                                                                FramebufferCache& cache,
                                                                const rhi::IRenderPass& pass,
                                                                std::span<const rhi::ITexture* const>
                                                                    attachments,
                                                                u32 width, u32 height,
                                                                std::string name);
    void RecordBatches(graph::PassContext& context, const std::vector<Batch>& batches);
    Result<void> UploadInstances(rhi::IDevice& device);
    void UploadUniforms(rhi::IDevice& device);
    void BuildBatches(std::span<const DrawItem> items);
    void ComputeCascades(const FrameView& view, const DirectionalLight& sun);

    RendererSettings settings_;
    bool initialized_ = false;

    rhi::PipelinePtr forwardPipeline_;
    rhi::PipelinePtr shadowPipeline_;
    /// Depth-only pipeline for the prepass.  SSAO needs depth before shading, so
    /// the depth is produced by its own pass rather than by the forward pass.
    rhi::PipelinePtr prepassPipeline_;
    rhi::PipelinePtr tonemapPipeline_;
    rhi::PipelinePtr bloomDownPipeline_;
    rhi::PipelinePtr bloomUpPipeline_;
    rhi::PipelinePtr ssaoPipeline_;
    rhi::DescriptorSetLayoutPtr descriptorLayout_;
    rhi::DescriptorSetPtr frameDescriptorSet_;
    rhi::SamplerPtr linearSampler_;
    rhi::BufferPtr uniformBuffer_;
    rhi::BufferPtr instanceBuffer_;
    rhi::RenderPassPtr forwardRenderPass_;
    rhi::RenderPassPtr shadowRenderPass_;
    rhi::RenderPassPtr tonemapRenderPass_;

    std::vector<GpuMesh> meshes_;
    std::vector<GpuInstanceData> instances_;
    std::vector<Batch> batches_;
    std::vector<Batch> shadowBatches_;
    FramebufferCache forwardFramebuffer_;
    FramebufferCache shadowFramebuffer_;
    FramebufferCache prepassFramebuffer_;
    FramebufferCache tonemapFramebuffer_;
    std::vector<ShadowCascade> cascades_;
    CullResult cullResult_;
    FrameUniforms uniforms_{};
    RendererStats stats_{};
    graph::RenderGraph graph_;
    graph::TextureHandle hdrColor_;
    graph::TextureHandle sceneDepth_;
    graph::TextureHandle output_;
    graph::TextureHandle shadowMap_;
    graph::TextureHandle ssaoTarget_;
    std::vector<graph::TextureHandle> bloomDown_;
    std::vector<graph::TextureHandle> bloomUp_;
    bool graphResourcesValid_ = false;
    rhi::ITexture* importedPresentTarget_ = nullptr;
    FrameView lastView_;
};

} // namespace l3d::render
