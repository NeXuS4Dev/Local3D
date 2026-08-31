#pragma once
/// @file null/NullDevice.hpp
/// @brief The reference RHI backend: a CPU only implementation that models GPU
///        semantics and validates every call.
///
/// Why this exists (docs/architecture/rhi.md):
///  * It lets the renderer, render graph and asset pipeline run - and be tested -
///    on machines without a GPU, including CI.
///  * It is the specification of the RHI's contracts: buffer usage flags, render
///    pass compatibility, descriptor layout matching, resource lifetime.  The
///    Vulkan backend is held to the same rules.
///  * Draw calls are counted, not executed, so tests can assert on renderer
///    behaviour (culling, batching, pass ordering) deterministically.

#include "local3d/rhi/RhiDevice.hpp"

#include <mutex>
#include <vector>

namespace l3d::rhi::null {

/// Installs the null backend's device factory.  Called by CreateDevice; safe to
/// call repeatedly.
void RegisterNullBackend();


class NullDevice;

class NullDevice final : public IDevice {
public:
    explicit NullDevice(const DeviceDesc& desc);
    ~NullDevice() override;

    [[nodiscard]] const DeviceInfo& Info() const noexcept override { return info_; }
    [[nodiscard]] u32 FrameIndex() const noexcept override { return frameIndex_; }
    [[nodiscard]] u32 FrameCount() const noexcept override { return frameCount_; }
    [[nodiscard]] u64 FrameNumber() const noexcept override { return frameNumber_; }

    Result<BufferPtr> CreateBuffer(const BufferDesc& desc) override;
    Result<TexturePtr> CreateTexture(const TextureDesc& desc) override;
    Result<SamplerPtr> CreateSampler(const SamplerDesc& desc) override;
    Result<ShaderModulePtr> CreateShaderModule(const ShaderModuleDesc& desc) override;
    Result<DescriptorSetLayoutPtr> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
    Result<DescriptorSetPtr> AllocateDescriptorSet(const IDescriptorSetLayout& layout) override;
    void WriteDescriptorSet(IDescriptorSet& set, std::span<const DescriptorWrite> writes) override;
    Result<RenderPassPtr> CreateRenderPass(const RenderPassDesc& desc) override;
    Result<FramebufferPtr> CreateFramebuffer(const IRenderPass& renderPass,
                                             std::span<const ITexture* const> attachments,
                                             u32 width, u32 height,
                                             std::string debugName) override;
    Result<PipelinePtr> CreateGraphicsPipeline(const PipelineDesc& desc) override;
    Result<PipelinePtr> CreateComputePipeline(const PipelineDesc& desc) override;
    Result<CommandBufferPtr> CreateCommandBuffer() override;
    Result<QueryPoolPtr> CreateTimestampQueryPool(u32 queryCount) override;
    Result<SwapchainPtr> CreateSwapchain(const SwapchainDesc& desc) override;

    void UpdateBuffer(IBuffer& buffer, u64 offset, ConstByteSpan data) override;
    void UpdateTexture(ITexture& texture, std::span<const ConstByteSpan> mipData) override;

    void Submit(ICommandBuffer& commandBuffer) override;
    void BeginFrame() override;
    void EndFrame() override;
    void WaitIdle() override;

    std::vector<f64> ReadTimestamps(const IQueryPool& pool, u32 firstQuery, u32 count) override;
    void ScheduleRelease(GpuResource* resource) override;

    MemoryReport MemoryUsage() const override;
    [[nodiscard]] u64 ValidationErrorCount() const noexcept override { return validationErrors_.size(); }
    std::vector<std::string> ValidationErrors() const override;

    /// Clear recorded errors (tests assert on a clean slate per case).
    void ClearValidationErrors();

    [[nodiscard]] u64 NextResourceId() noexcept { return nextResourceId_++; }
    void ReportValidationError(std::string message);
    void TrackAllocation(u64 bytes, bool isTexture);
    void TrackFree(u64 bytes);
    void UntrackResource(bool isTexture);

    /// Frames each resource must survive after release before it is freed.
    [[nodiscard]] u32 FrameLatency() const noexcept { return frameCount_; }
    [[nodiscard]] u64 SubmittedCommandBuffers() const noexcept { return submittedCommandBuffers_; }

private:
    struct DeferredEntry {
        GpuResource* resource = nullptr;
        u64 releaseFrame = 0;
    };

    void ProcessDeferredDeletions();

    DeviceDesc desc_;
    DeviceInfo info_;
    u32 frameCount_ = 2;
    u32 frameIndex_ = 0;
    u64 frameNumber_ = 0;
    u64 nextResourceId_ = 1;
    u64 submittedCommandBuffers_ = 0;

    std::vector<DeferredEntry> deferred_;
    mutable std::mutex validationMutex_;
    std::vector<std::string> validationErrors_;

    u64 bufferBytes_ = 0;
    u64 textureBytes_ = 0;
    u32 bufferCount_ = 0;
    u32 textureCount_ = 0;
    u32 liveResources_ = 0;
    u64 totalAllocatedBytes_ = 0;
    u64 totalFreedBytes_ = 0;
};

} // namespace l3d::rhi::null
