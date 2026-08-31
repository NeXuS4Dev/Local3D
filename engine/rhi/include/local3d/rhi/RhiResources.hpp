#pragma once
/// @file RhiResources.hpp
/// @brief GPU object interfaces and their ownership model.
///
/// Ownership model (important):
///  * Engine code owns GPU objects through `ResourcePtr<T>` (a unique_ptr with a
///    custom deleter).  There are no raw owning pointers anywhere.
///  * The deleter does **not** call delete.  It hands the object back to the
///    device, which schedules destruction for a later frame.  GPU work already
///    in flight may still reference the object, so freeing it immediately would
///    be a use-after-free on the GPU.  See DeferredDeleter below.
///  * `IDevice::EndFrame()` releases everything whose last-use frame has
///    completed.

#include "local3d/core/Common.hpp"
#include "local3d/rhi/RhiTypes.hpp"

#include <memory>
#include <string>
#include <utility>

namespace l3d::rhi {

/// Common base for every GPU object.  Backends derive from these interfaces and
/// never expose their native handles in public headers.
class IDevice;

class GpuResource {
public:
    virtual ~GpuResource() = default;

    /// Backend specific handle value, for debugging only (never dereferenced by
    /// engine code).  Backends override it to expose e.g. a VkBuffer.
    [[nodiscard]] virtual u64 NativeHandle() const noexcept { return resourceId_; }

    [[nodiscard]] virtual std::string_view DebugName() const noexcept { return debugName_; }

    /// Unique per-device id, used by the frame debugger and validation.
    [[nodiscard]] u64 ResourceId() const noexcept { return resourceId_; }

    /// Hand the object back to its device for deferred destruction.  Called by
    /// ResourcePtr's deleter; it never frees the object directly.
    void Release() noexcept;

    /// The device that owns this resource (set by the backend at construction).
    [[nodiscard]] IDevice* Owner() const noexcept { return device_; }

    /// Backends may refine the name once more context is known (for example a
    /// command buffer named by the pass that recorded it).
    void SetDebugName(std::string name) { debugName_ = std::move(name); }

protected:
    GpuResource(u64 resourceId, std::string debugName)
        : resourceId_(resourceId), debugName_(std::move(debugName)) {}

    void AttachDevice(IDevice* device) noexcept { device_ = device; }

private:
    u64 resourceId_;
    std::string debugName_;
    IDevice* device_ = nullptr;
};

/// Deleter that returns ownership to the device instead of freeing memory.
struct GpuResourceDeleter {
    void operator()(GpuResource* resource) const noexcept;
};

template <typename T>
using ResourcePtr = std::unique_ptr<T, GpuResourceDeleter>;

using BufferPtr = ResourcePtr<class IBuffer>;
using TexturePtr = ResourcePtr<class ITexture>;
using SamplerPtr = ResourcePtr<class ISampler>;
using ShaderModulePtr = ResourcePtr<class IShaderModule>;
using DescriptorSetLayoutPtr = ResourcePtr<class IDescriptorSetLayout>;
using DescriptorSetPtr = ResourcePtr<class IDescriptorSet>;
using RenderPassPtr = ResourcePtr<class IRenderPass>;
using FramebufferPtr = ResourcePtr<class IFramebuffer>;
using PipelinePtr = ResourcePtr<class IPipeline>;
using CommandBufferPtr = ResourcePtr<class ICommandBuffer>;
using QueryPoolPtr = ResourcePtr<class IQueryPool>;
using SwapchainPtr = ResourcePtr<class ISwapchain>;

class IBuffer : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IBuffer(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual u64 Size() const noexcept = 0;
    [[nodiscard]] virtual BufferUsage Usage() const noexcept = 0;
    [[nodiscard]] virtual MemoryType Memory() const noexcept = 0;

    /// CPU visible memory can be mapped; otherwise this returns nullptr.
    [[nodiscard]] virtual void* Map() = 0;
    virtual void Unmap() = 0;
};

class ITexture : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    ITexture(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual const TextureDesc& Desc() const noexcept = 0;
    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;
    [[nodiscard]] virtual Format GetFormat() const noexcept = 0;
    /// Total GPU memory footprint in bytes (all mips and layers).
    [[nodiscard]] virtual u64 SizeInBytes() const noexcept = 0;
};

class ISampler : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    ISampler(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual const SamplerDesc& Desc() const noexcept = 0;
};

class IShaderModule : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IShaderModule(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual ShaderStage Stage() const noexcept = 0;
    [[nodiscard]] virtual ShaderFormat GetFormat() const noexcept = 0;
    [[nodiscard]] virtual usize BytecodeSize() const noexcept = 0;
};

class IDescriptorSetLayout : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IDescriptorSetLayout(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual const DescriptorSetLayoutDesc& Desc() const noexcept = 0;
    /// Stable id referenced by PipelineDesc::descriptorSetLayoutIds.
    [[nodiscard]] u64 LayoutId() const noexcept { return ResourceId(); }
};

/// One write into a descriptor set.  Points at engine owned resources; the
/// backend keeps them alive for the lifetime of the set (documented: callers
/// must not destroy a resource still referenced by a live descriptor set).
struct DescriptorWrite {
    u32 binding = 0;
    u32 arrayIndex = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    const IBuffer* buffer = nullptr;
    u64 bufferOffset = 0;
    u64 bufferRange = 0; ///< 0 means "whole buffer".
    const ITexture* texture = nullptr;
    u32 baseMipLevel = 0;
    u32 mipLevelCount = 1;
    const ISampler* sampler = nullptr;
};

class IDescriptorSet : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IDescriptorSet(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual const IDescriptorSetLayout& Layout() const noexcept = 0;
    [[nodiscard]] virtual usize WriteCount() const noexcept = 0;
};

class IRenderPass : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IRenderPass(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual const RenderPassDesc& Desc() const noexcept = 0;
};

class IFramebuffer : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IFramebuffer(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;
    [[nodiscard]] virtual usize AttachmentCount() const noexcept = 0;
    [[nodiscard]] virtual const IRenderPass& RenderPass() const noexcept = 0;
};

class IPipeline : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IPipeline(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual const PipelineDesc& Desc() const noexcept = 0;
    [[nodiscard]] virtual bool IsCompute() const noexcept = 0;
};

class IQueryPool : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    IQueryPool(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual u32 QueryCount() const noexcept = 0;
};

class ISwapchain : public GpuResource {
public:
    /// Backends forward the resource id and debug name to GpuResource.
    ISwapchain(u64 resourceId, std::string debugName)
        : GpuResource(resourceId, std::move(debugName)) {}
    [[nodiscard]] virtual u32 ImageCount() const noexcept = 0;
    [[nodiscard]] virtual u32 CurrentImageIndex() const noexcept = 0;
    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;
    [[nodiscard]] virtual Format GetFormat() const noexcept = 0;

    /// Acquire the next image.  `outSuboptimal` reports a resize is pending.
    [[nodiscard]] virtual bool AcquireNextImage(bool& outSuboptimal) = 0;
    [[nodiscard]] virtual const ITexture& CurrentImage() const noexcept = 0;
    virtual void Present() = 0;
};

} // namespace l3d::rhi
