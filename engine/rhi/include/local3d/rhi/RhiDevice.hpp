#pragma once
/// @file RhiDevice.hpp
/// @brief Device and command buffer interfaces - the engine's only view of the GPU.
///
/// Frame model:
///   BeginFrame()  - advance the frame index, free resources whose GPU work has
///                   completed, acquire the swapchain image.
///   ... record command buffers ...
///   Submit()      - hand command buffers to the GPU.
///   EndFrame()    - present and mark the frame as in flight.
///
/// `FrameCount()` is the number of frames buffered (2 or 3).  Per-frame CPU
/// data must be written into the slot returned by FrameIndex().

#include "local3d/core/Result.hpp"
#include "local3d/rhi/RhiResources.hpp"

#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace l3d::rhi {

/// One command buffer.  Recording is single threaded by contract: a command
/// buffer belongs to the thread that began it until it is submitted.
class ICommandBuffer : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    ICommandBuffer(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}

    enum class Level : u8 { Primary, Secondary };

    virtual void Begin(std::string_view debugName = {}) = 0;
    virtual void End() = 0;

    // --- Render passes ----------------------------------------------------
    virtual void BeginRenderPass(const IRenderPass& renderPass, const IFramebuffer& framebuffer,
                                 std::span<const ClearValue> clearValues) = 0;
    virtual void EndRenderPass() = 0;
    [[nodiscard]] virtual bool IsInsideRenderPass() const noexcept = 0;

    // --- State ------------------------------------------------------------
    virtual void BindPipeline(const IPipeline& pipeline) = 0;
    virtual void BindVertexBuffer(u32 binding, const IBuffer& buffer, u64 offset = 0) = 0;
    virtual void BindIndexBuffer(const IBuffer& buffer, u64 offset, bool indices32Bit) = 0;
    virtual void BindDescriptorSet(u32 setIndex, const IDescriptorSetLayout& layout,
                                   const IDescriptorSet& set) = 0;
    virtual void PushConstants(ShaderStage stage, u32 offset, ConstByteSpan data) = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const Rect2D& scissor) = 0;

    // --- Draws ------------------------------------------------------------
    virtual void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0,
                      u32 firstInstance = 0) = 0;
    virtual void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                             i32 vertexOffset = 0, u32 firstInstance = 0) = 0;
    /// Indirect draw: arguments come from a GPU buffer (GPU driven rendering).
    virtual void DrawIndexedIndirect(const IBuffer& argumentBuffer, u64 offset, u32 drawCount,
                                     u32 stride) = 0;

    // --- Compute ----------------------------------------------------------
    virtual void Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) = 0;

    // --- Transfers --------------------------------------------------------
    virtual void CopyBuffer(const IBuffer& src, u64 srcOffset, IBuffer& dst, u64 dstOffset,
                            u64 size) = 0;
    struct TextureCopyRegion {
        u32 mipLevel = 0;
        u32 arrayLayer = 0;
        u32 x = 0;
        u32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };
    virtual void CopyBufferToTexture(const IBuffer& src, u64 srcOffset, u64 rowPitch,
                                     ITexture& dst, const TextureCopyRegion& region) = 0;
    virtual void GenerateMipmaps(ITexture& texture) = 0;

    // --- Queries and debugging -------------------------------------------
    virtual void WriteTimestamp(const IQueryPool& pool, u32 queryIndex) = 0;
    virtual void BeginDebugLabel(std::string_view name, math::Color color = {}) = 0;
    virtual void EndDebugLabel() = 0;

    // --- Recorded work accounting (used by tests and the frame debugger) --
    struct Stats {
        u32 drawCalls = 0;
        u32 indexedDrawCalls = 0;
        u32 indirectDrawCalls = 0;
        u32 dispatches = 0;
        u32 renderPasses = 0;
        u32 copies = 0;
        u32 pipelineBinds = 0;
        u32 descriptorSetBinds = 0;
        u32 pushConstantWrites = 0;
        u32 timestamps = 0;
    };
    [[nodiscard]] virtual const Stats& GetStats() const noexcept = 0;
};

/// Device creation parameters.
struct DeviceDesc {
    std::string applicationName = "Local3D";
    BackendType preferredBackend = BackendType::Null;
    bool enableValidation = true;
    bool enableDebugMarkers = true;
    /// Frames buffered.  2 is the usual latency/throughput trade off; 3 helps on
    /// displays with uneven frame pacing.
    u32 frameCount = 2;
    /// Window handle for swapchain creation (ignored by the null backend).
    void* windowHandle = nullptr;
    /// When true the device is created without any surface (tools, tests).
    bool headless = true;
};

/// Result of a successful device creation.
struct DeviceHandle {
    std::unique_ptr<class IDevice> device;
};

class IDevice {
public:
    virtual ~IDevice() = default;

    // --- Info -------------------------------------------------------------
    [[nodiscard]] virtual const DeviceInfo& Info() const noexcept = 0;
    [[nodiscard]] virtual u32 FrameIndex() const noexcept = 0;
    [[nodiscard]] virtual u32 FrameCount() const noexcept = 0;
    [[nodiscard]] virtual u64 FrameNumber() const noexcept = 0;

    // --- Resource creation ------------------------------------------------
    [[nodiscard]] virtual Result<BufferPtr> CreateBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual Result<TexturePtr> CreateTexture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual Result<SamplerPtr> CreateSampler(const SamplerDesc& desc) = 0;
    [[nodiscard]] virtual Result<ShaderModulePtr> CreateShaderModule(const ShaderModuleDesc& desc) = 0;
    [[nodiscard]] virtual Result<DescriptorSetLayoutPtr>
    CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) = 0;
    [[nodiscard]] virtual Result<DescriptorSetPtr>
    AllocateDescriptorSet(const IDescriptorSetLayout& layout) = 0;
    virtual void WriteDescriptorSet(IDescriptorSet& set, std::span<const DescriptorWrite> writes) = 0;
    [[nodiscard]] virtual Result<RenderPassPtr> CreateRenderPass(const RenderPassDesc& desc) = 0;
    [[nodiscard]] virtual Result<FramebufferPtr>
    CreateFramebuffer(const IRenderPass& renderPass, std::span<const ITexture* const> attachments,
                      u32 width, u32 height, std::string debugName = {}) = 0;
    [[nodiscard]] virtual Result<PipelinePtr> CreateGraphicsPipeline(const PipelineDesc& desc) = 0;
    [[nodiscard]] virtual Result<PipelinePtr> CreateComputePipeline(const PipelineDesc& desc) = 0;
    [[nodiscard]] virtual Result<CommandBufferPtr> CreateCommandBuffer() = 0;
    [[nodiscard]] virtual Result<QueryPoolPtr> CreateTimestampQueryPool(u32 queryCount) = 0;
    [[nodiscard]] virtual Result<SwapchainPtr> CreateSwapchain(const SwapchainDesc& desc) = 0;

    // --- Uploads ----------------------------------------------------------
    /// Stage data into a buffer.  Backends choose the mechanism (direct map for
    /// host visible memory, staging buffer + copy otherwise).
    virtual void UpdateBuffer(IBuffer& buffer, u64 offset, ConstByteSpan data) = 0;
    /// Upload a full mip chain for a 2D texture from tightly packed data.
    virtual void UpdateTexture(ITexture& texture, std::span<const ConstByteSpan> mipData) = 0;

    // --- Execution --------------------------------------------------------
    /// Queue a *finished* command buffer (Begin ... End) for execution.  A
    /// buffer that is still recording, was never begun, or has already been
    /// submitted is a validation error; re-record it before resubmitting.
    virtual void Submit(ICommandBuffer& commandBuffer) = 0;
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void WaitIdle() = 0;

    /// Read back timestamp queries as nanoseconds.  Empty on failure.
    [[nodiscard]] virtual std::vector<f64> ReadTimestamps(const IQueryPool& pool, u32 firstQuery,
                                                          u32 count) = 0;

    // --- Deferred destruction --------------------------------------------
    /// Called by GpuResourceDeleter.  The device keeps the object alive until
    /// the frame that last used it has completed.
    virtual void ScheduleRelease(GpuResource* resource) = 0;

    // --- Diagnostics ------------------------------------------------------
    /// Contract for backends: a malformed descriptor is reported as an error
    /// Result from the creation call, while *misuse of a valid object* (drawing
    /// outside a render pass, uploading past the end of a buffer, binding a
    /// buffer without the required usage flag) is recorded here and reported
    /// through the log.  Validation is enabled by DeviceDesc::enableValidation.
    struct MemoryReport {
        u64 bufferBytes = 0;
        u64 textureBytes = 0;
        u32 bufferCount = 0;
        u32 textureCount = 0;
        u32 liveResources = 0;
        u32 deferredResources = 0;
        u64 totalAllocatedBytes = 0;
        u64 totalFreedBytes = 0;
    };
    [[nodiscard]] virtual MemoryReport MemoryUsage() const = 0;
    [[nodiscard]] virtual u64 ValidationErrorCount() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::string> ValidationErrors() const = 0;
};

/// Create a device.  Falls back to the null backend if the requested one is
/// unavailable, reporting the substitution through `usedFallback`.
[[nodiscard]] Result<std::unique_ptr<IDevice>> CreateDevice(const DeviceDesc& desc,
                                                            bool* usedFallback = nullptr);

/// Register a backend factory (used by backends compiled as plugins).
using DeviceFactory = std::function<Result<std::unique_ptr<IDevice>>(const DeviceDesc&)>;
void RegisterDeviceFactory(BackendType type, DeviceFactory factory);

} // namespace l3d::rhi
