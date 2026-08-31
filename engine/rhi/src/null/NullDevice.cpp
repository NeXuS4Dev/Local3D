#include "null/NullDevice.hpp"

#include "local3d/core/Assert.hpp"
#include "local3d/core/Format.hpp"
#include "local3d/core/Log.hpp"
#include "local3d/core/Time.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

// The null backend is long but intentionally straightforward: every method does
// the validation the Vulkan backend is expected to do, then records the effect.
// NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)

namespace l3d::rhi::null {
namespace {

[[nodiscard]] constexpr u32 MipLevelsFor(u32 width, u32 height) noexcept {
    u32 levels = 1;
    while (width > 1 || height > 1) {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        ++levels;
    }
    return levels;
}

/// Downcast helper used throughout: the null backend only ever passes its own
/// objects through the public interfaces.
template <typename T, typename Base>
[[nodiscard]] T& As(Base& base) {
    return static_cast<T&>(base);
}

template <typename T, typename Base>
[[nodiscard]] const T& As(const Base& base) {
    return static_cast<const T&>(base);
}

/// Adds the null backend's shared state (a device reference) on top of any RHI
/// interface.  Identity, names and deferred release live in GpuResource.
template <typename Interface>
class NullObject : public Interface {
public:
    NullObject(NullDevice& device, u64 id, std::string debugName)
        : Interface(id, std::move(debugName)), device_(device) {
        this->AttachDevice(&device);
    }

protected:
    NullDevice& device_;
};

// --- Resource implementations ----------------------------------------------

class NullBuffer final : public NullObject<IBuffer> {
public:
    NullBuffer(NullDevice& device, const BufferDesc& desc, u64 id)
        : NullObject<IBuffer>(device, id, desc.debugName), desc_(desc), storage_(static_cast<usize>(desc.size)) {}


    [[nodiscard]] u64 Size() const noexcept override { return desc_.size; }
    [[nodiscard]] BufferUsage Usage() const noexcept override { return desc_.usage; }
    [[nodiscard]] MemoryType Memory() const noexcept override { return desc_.memory; }

    [[nodiscard]] void* Map() override {
        mapped_ = true;
        return storage_.data();
    }

    void Unmap() override { mapped_ = false; }
    [[nodiscard]] bool IsMapped() const noexcept { return mapped_; }
    [[nodiscard]] std::vector<std::byte>& Storage() noexcept { return storage_; }

private:
    BufferDesc desc_;
    std::vector<std::byte> storage_;
    bool mapped_ = false;
};

class NullTexture final : public NullObject<ITexture> {
public:
    NullTexture(NullDevice& device, const TextureDesc& desc, u64 id)
        : NullObject<ITexture>(device, id, desc.debugName), desc_(desc) {}


    [[nodiscard]] const TextureDesc& Desc() const noexcept override { return desc_; }
    [[nodiscard]] u32 Width() const noexcept override { return desc_.width; }
    [[nodiscard]] u32 Height() const noexcept override { return desc_.height; }
    [[nodiscard]] Format GetFormat() const noexcept override { return desc_.format; }
    [[nodiscard]] u64 SizeInBytes() const noexcept override { return sizeInBytes_; }
    void SetSizeInBytes(u64 bytes) noexcept { sizeInBytes_ = bytes; }

    /// Mip data captured by UpdateTexture, so importers can be tested end to end.
    [[nodiscard]] const std::vector<std::vector<std::byte>>& MipData() const noexcept {
        return mipData_;
    }
    void SetMipData(std::vector<std::vector<std::byte>> data) { mipData_ = std::move(data); }

private:
    TextureDesc desc_;
    u64 sizeInBytes_ = 0;
    std::vector<std::vector<std::byte>> mipData_;
};

class NullSampler final : public NullObject<ISampler> {
public:
    NullSampler(NullDevice& device, const SamplerDesc& desc, u64 id)
        : NullObject<ISampler>(device, id, desc.debugName), desc_(desc) {}

    [[nodiscard]] const SamplerDesc& Desc() const noexcept override { return desc_; }

private:
    SamplerDesc desc_;
};

class NullShaderModule final : public NullObject<IShaderModule> {
public:
    NullShaderModule(NullDevice& device, const ShaderModuleDesc& desc, u64 id)
        : NullObject<IShaderModule>(device, id, desc.debugName),
          stage_(desc.stage), format_(desc.format), bytecodeSize_(desc.bytecode.size()),
          entryPoint_(desc.entryPoint), bytecode_(desc.bytecode.begin(), desc.bytecode.end()) {}


    [[nodiscard]] ShaderStage Stage() const noexcept override { return stage_; }
    [[nodiscard]] ShaderFormat GetFormat() const noexcept override { return format_; }
    [[nodiscard]] usize BytecodeSize() const noexcept override { return bytecodeSize_; }
    [[nodiscard]] const std::string& EntryPoint() const noexcept { return entryPoint_; }
    [[nodiscard]] const std::vector<std::byte>& Bytecode() const noexcept { return bytecode_; }

private:
    ShaderStage stage_;
    ShaderFormat format_;
    usize bytecodeSize_;
    std::string entryPoint_;
    std::vector<std::byte> bytecode_;
};

class NullDescriptorSetLayout final : public NullObject<IDescriptorSetLayout> {
public:
    NullDescriptorSetLayout(NullDevice& device, const DescriptorSetLayoutDesc& desc, u64 id)
        : NullObject<IDescriptorSetLayout>(device, id, desc.debugName), desc_(desc) {}

    [[nodiscard]] const DescriptorSetLayoutDesc& Desc() const noexcept override { return desc_; }

private:
    DescriptorSetLayoutDesc desc_;
};

class NullDescriptorSet final : public NullObject<IDescriptorSet> {
public:
    NullDescriptorSet(NullDevice& device, const NullDescriptorSetLayout& layout, u64 id)
        : NullObject<IDescriptorSet>(device, id, fmt::Format("descriptor-set-{}", id)), layout_(layout) {}

    [[nodiscard]] const IDescriptorSetLayout& Layout() const noexcept override { return layout_; }
    [[nodiscard]] usize WriteCount() const noexcept override { return writes_.size(); }
    void RecordWrite(const DescriptorWrite& write) { writes_.push_back(write); }
    [[nodiscard]] const std::vector<DescriptorWrite>& Writes() const noexcept { return writes_; }

private:
    const NullDescriptorSetLayout& layout_;
    std::vector<DescriptorWrite> writes_;
};

class NullRenderPass final : public NullObject<IRenderPass> {
public:
    NullRenderPass(NullDevice& device, const RenderPassDesc& desc, u64 id)
        : NullObject<IRenderPass>(device, id, desc.debugName), desc_(desc) {}

    [[nodiscard]] const RenderPassDesc& Desc() const noexcept override { return desc_; }

private:
    RenderPassDesc desc_;
};

class NullFramebuffer final : public NullObject<IFramebuffer> {
public:
    NullFramebuffer(NullDevice& device, const NullRenderPass& renderPass,
                    std::span<const ITexture* const> attachments, u32 width, u32 height,
                    std::string debugName, u64 id)
        : NullObject<IFramebuffer>(device, id, std::move(debugName)), renderPass_(renderPass),
          attachments_(attachments.begin(), attachments.end()), width_(width), height_(height) {}


    [[nodiscard]] u32 Width() const noexcept override { return width_; }
    [[nodiscard]] u32 Height() const noexcept override { return height_; }
    [[nodiscard]] usize AttachmentCount() const noexcept override { return attachments_.size(); }
    [[nodiscard]] const IRenderPass& RenderPass() const noexcept override { return renderPass_; }
    [[nodiscard]] const std::vector<const ITexture*>& Attachments() const noexcept {
        return attachments_;
    }

private:
    const NullRenderPass& renderPass_;
    std::vector<const ITexture*> attachments_;
    u32 width_;
    u32 height_;
};

class NullPipeline final : public NullObject<IPipeline> {
public:
    NullPipeline(NullDevice& device, const PipelineDesc& desc, bool compute, u64 id)
        : NullObject<IPipeline>(device, id, desc.debugName), desc_(desc), compute_(compute) {}

    [[nodiscard]] const PipelineDesc& Desc() const noexcept override { return desc_; }
    [[nodiscard]] bool IsCompute() const noexcept override { return compute_; }

private:
    PipelineDesc desc_;
    bool compute_;
};

class NullQueryPool final : public NullObject<IQueryPool> {
public:
    NullQueryPool(NullDevice& device, u32 queryCount, u64 id)
        : NullObject<IQueryPool>(device, id, fmt::Format("timestamp-pool-{}", id)),
          timestamps_(queryCount, 0), resolved_(queryCount, false) {}

    [[nodiscard]] u32 QueryCount() const noexcept override { return static_cast<u32>(timestamps_.size()); }

    void Write(u32 index, u64 timestampNs) const {
        if (index < timestamps_.size()) {
            timestamps_[index] = timestampNs;
            resolved_[index] = true;
        }
    }

    [[nodiscard]] u64 Read(u32 index) const {
        return index < timestamps_.size() && resolved_[index] ? timestamps_[index] : 0;
    }

private:
    mutable std::vector<u64> timestamps_;
    mutable std::vector<bool> resolved_;
};

class NullSwapchain final : public NullObject<ISwapchain> {
public:
    NullSwapchain(NullDevice& device, const SwapchainDesc& desc, u64 id)
        : NullObject<ISwapchain>(device, id, "null-swapchain"), desc_(desc) {
        const u32 count = desc.imageCount > 0 ? desc.imageCount : 2;
        for (u32 i = 0; i < count; ++i) {
            TextureDesc textureDesc;
            textureDesc.width = desc.width;
            textureDesc.height = desc.height;
            textureDesc.format = desc.format;
            textureDesc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
            textureDesc.debugName = fmt::Format("swapchain-image-{}", i);
            images_.push_back(std::make_unique<NullTexture>(device, textureDesc,
                                                            device.NextResourceId()));
        }
    }


    [[nodiscard]] u32 ImageCount() const noexcept override { return static_cast<u32>(images_.size()); }
    [[nodiscard]] u32 CurrentImageIndex() const noexcept override { return currentIndex_; }
    [[nodiscard]] u32 Width() const noexcept override { return desc_.width; }
    [[nodiscard]] u32 Height() const noexcept override { return desc_.height; }
    [[nodiscard]] Format GetFormat() const noexcept override { return desc_.format; }

    bool AcquireNextImage(bool& outSuboptimal) override {
        if (images_.empty()) {
            return false;
        }
        currentIndex_ = (currentIndex_ + 1) % static_cast<u32>(images_.size());
        outSuboptimal = false;
        ++acquireCount_;
        return true;
    }

    [[nodiscard]] const ITexture& CurrentImage() const noexcept override { return *images_[currentIndex_]; }

    void Present() override { ++presentCount_; }
    [[nodiscard]] u64 PresentCount() const noexcept { return presentCount_; }
    [[nodiscard]] u64 AcquireCount() const noexcept { return acquireCount_; }

private:
    SwapchainDesc desc_;
    std::vector<std::unique_ptr<NullTexture>> images_;
    u32 currentIndex_ = 0;
    u64 presentCount_ = 0;
    u64 acquireCount_ = 0;
};

// --- Command buffer --------------------------------------------------------

class NullCommandBuffer final : public NullObject<ICommandBuffer> {
public:
    NullCommandBuffer(NullDevice& device, u64 id)
        : NullObject<ICommandBuffer>(device, id, fmt::Format("command-buffer-{}", id)) {}


    void Begin(std::string_view debugName) override {
        if (begun_) {
            device_.ReportValidationError("CommandBuffer::Begin called twice");
            return;
        }
        if (!debugName.empty()) {
            SetDebugName(std::string(debugName));
        }
        begun_ = true;
        finished_ = false;
        stats_ = Stats{};
    }

    void End() override {
        if (!begun_) {
            device_.ReportValidationError("CommandBuffer::End without Begin");
            return;
        }
        if (insideRenderPass_) {
            device_.ReportValidationError("CommandBuffer::End while a render pass is open");
        }
        if (debugLabelDepth_ > 0) {
            device_.ReportValidationError("CommandBuffer::End with unbalanced debug labels");
        }
        begun_ = false;
        // Only a cleanly closed recording may be submitted.
        finished_ = !insideRenderPass_ && debugLabelDepth_ == 0;
    }

    void BeginRenderPass(const IRenderPass& renderPass, const IFramebuffer& framebuffer,
                         std::span<const ClearValue> clearValues) override {
        if (!CheckBegun()) {
            return;
        }
        if (insideRenderPass_) {
            device_.ReportValidationError("BeginRenderPass while already inside a render pass");
            return;
        }
        const auto& pass = As<NullRenderPass>(renderPass);
        const auto& buffer = As<NullFramebuffer>(framebuffer);
        if (&buffer.RenderPass() != &pass) {
            device_.ReportValidationError("Framebuffer was created with a different render pass");
        }
        const usize expected = pass.Desc().colorAttachments.Size() +
                               (pass.Desc().hasDepthStencil ? 1 : 0);
        if (buffer.AttachmentCount() != expected) {
            device_.ReportValidationError(fmt::Format(
                "Framebuffer has {} attachments but the render pass expects {}",
                buffer.AttachmentCount(), expected));
        }
        if (buffer.Width() == 0 || buffer.Height() == 0) {
            device_.ReportValidationError("Framebuffer has a zero sized attachment");
        }
        if (clearValues.size() < pass.Desc().colorAttachments.Size()) {
            device_.ReportValidationError("Not enough clear values for the color attachments");
        }
        // Validate attachment formats against the pass description.
        for (usize i = 0; i < pass.Desc().colorAttachments.Size() && i < buffer.Attachments().size(); ++i) {
            const ITexture* texture = buffer.Attachments()[i];
            if (texture != nullptr && texture->GetFormat() != pass.Desc().colorAttachments[i].format) {
                device_.ReportValidationError("Framebuffer color attachment format mismatch");
            }
        }
        insideRenderPass_ = true;
        activePass_ = &pass;
        boundPipeline_ = nullptr;
        ++stats_.renderPasses;
    }

    void EndRenderPass() override {
        if (!insideRenderPass_) {
            device_.ReportValidationError("EndRenderPass without BeginRenderPass");
            return;
        }
        insideRenderPass_ = false;
        activePass_ = nullptr;
        boundPipeline_ = nullptr;
    }

    [[nodiscard]] bool IsInsideRenderPass() const noexcept override { return insideRenderPass_; }

    void BindPipeline(const IPipeline& pipeline) override {
        if (!CheckBegun()) {
            return;
        }
        const auto& null = As<NullPipeline>(pipeline);
        if (null.IsCompute() == insideRenderPass_) {
            device_.ReportValidationError(insideRenderPass_
                                              ? "Compute pipeline bound inside a render pass"
                                              : "Graphics pipeline bound outside a render pass");
        }
        boundPipeline_ = &null;
        boundVertexBuffers_.clear();
        indexBufferBound_ = false;
        ++stats_.pipelineBinds;
    }

    void BindVertexBuffer(u32 binding, const IBuffer& buffer, u64 offset) override {
        if (!CheckBegun()) {
            return;
        }
        if (!HasAnyFlag(buffer.Usage(), BufferUsage::Vertex)) {
            device_.ReportValidationError("Buffer bound as vertex buffer lacks the Vertex usage flag");
        }
        if (offset > buffer.Size()) {
            device_.ReportValidationError("Vertex buffer offset is out of range");
        }
        boundVertexBuffers_.push_back(binding);
    }

    void BindIndexBuffer(const IBuffer& buffer, u64 offset, bool indices32Bit) override {
        if (!CheckBegun()) {
            return;
        }
        if (!HasAnyFlag(buffer.Usage(), BufferUsage::Index)) {
            device_.ReportValidationError("Buffer bound as index buffer lacks the Index usage flag");
        }
        if (offset > buffer.Size()) {
            device_.ReportValidationError("Index buffer offset is out of range");
        }
        indexBufferBound_ = true;
        indexBuffer32Bit_ = indices32Bit;
    }

    void BindDescriptorSet(u32 setIndex, const IDescriptorSetLayout& layout,
                           const IDescriptorSet& set) override {
        if (!CheckBegun()) {
            return;
        }
        if (&set.Layout() != &layout) {
            device_.ReportValidationError("Descriptor set layout does not match the set");
        }
        if (boundPipeline_ != nullptr) {
            const auto& layouts = boundPipeline_->Desc().descriptorSetLayoutIds;
            if (setIndex >= layouts.Size()) {
                device_.ReportValidationError(
                    "Descriptor set index is not declared by the bound pipeline");
            } else if (layouts[setIndex] != layout.ResourceId()) {
                device_.ReportValidationError(
                    "Descriptor set layout does not match the pipeline layout");
            }
        }
        boundDescriptorSets_[setIndex] = set.ResourceId();
        ++stats_.descriptorSetBinds;
    }

    void PushConstants(ShaderStage stage, u32 offset, ConstByteSpan data) override {
        if (!CheckBegun()) {
            return;
        }
        if (boundPipeline_ == nullptr) {
            device_.ReportValidationError("PushConstants before BindPipeline");
            return;
        }
        bool declared = false;
        for (const PushConstantRange& range : boundPipeline_->Desc().pushConstantRanges) {
            if (range.stage == stage && offset >= range.offset &&
                offset + data.size() <= range.offset + range.size) {
                declared = true;
            }
        }
        if (!declared) {
            device_.ReportValidationError("PushConstants outside the pipeline's declared ranges");
        }
        if (data.size() > device_.Info().maxPushConstantSize) {
            device_.ReportValidationError("PushConstants larger than the device limit");
        }
        ++stats_.pushConstantWrites;
    }

    void SetViewport(const Viewport& viewport) override {
        if (!CheckBegun()) {
            return;
        }
        if (viewport.width <= 0.0f || viewport.height == 0.0f) {
            device_.ReportValidationError("Viewport has a non-positive width");
        }
        viewport_ = viewport;
        viewportSet_ = true;
    }

    void SetScissor(const Rect2D& scissor) override {
        if (!CheckBegun()) {
            return;
        }
        scissor_ = scissor;
    }

    void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override {
        if (!CheckDrawPreconditions("Draw")) {
            return;
        }
        if (vertexCount == 0 || instanceCount == 0) {
            device_.ReportValidationError("Draw with a zero vertex or instance count");
        }
        ValidateVertexBindings();
        ++stats_.drawCalls;
        L3D_UNUSED(firstVertex);
        L3D_UNUSED(firstInstance);
    }

    void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                     u32 firstInstance) override {
        if (!CheckDrawPreconditions("DrawIndexed")) {
            return;
        }
        if (!indexBufferBound_) {
            device_.ReportValidationError("DrawIndexed without an index buffer bound");
        }
        if (indexCount == 0 || instanceCount == 0) {
            device_.ReportValidationError("DrawIndexed with a zero index or instance count");
        }
        ValidateVertexBindings();
        ++stats_.indexedDrawCalls;
        L3D_UNUSED(firstIndex);
        L3D_UNUSED(vertexOffset);
        L3D_UNUSED(firstInstance);
    }

    void DrawIndexedIndirect(const IBuffer& argumentBuffer, u64 offset, u32 drawCount,
                             u32 stride) override {
        if (!CheckDrawPreconditions("DrawIndexedIndirect")) {
            return;
        }
        if (!HasAnyFlag(argumentBuffer.Usage(), BufferUsage::Indirect)) {
            device_.ReportValidationError("Indirect draw buffer lacks the Indirect usage flag");
        }
        if (stride == 0 || drawCount == 0) {
            device_.ReportValidationError("Indirect draw with a zero stride or count");
        }
        if (offset + static_cast<u64>(stride) * drawCount > argumentBuffer.Size()) {
            device_.ReportValidationError("Indirect draw arguments exceed the buffer size");
        }
        ++stats_.indirectDrawCalls;
    }

    void Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override {
        if (!CheckBegun()) {
            return;
        }
        if (insideRenderPass_) {
            device_.ReportValidationError("Dispatch inside a render pass");
        }
        if (boundPipeline_ == nullptr || !boundPipeline_->IsCompute()) {
            device_.ReportValidationError("Dispatch without a compute pipeline bound");
        }
        if (groupsX == 0 || groupsY == 0 || groupsZ == 0) {
            device_.ReportValidationError("Dispatch with a zero group count");
        }
        ++stats_.dispatches;
    }

    void CopyBuffer(const IBuffer& src, u64 srcOffset, IBuffer& dst, u64 dstOffset,
                    u64 size) override {
        if (!CheckBegun()) {
            return;
        }
        if (!HasAnyFlag(src.Usage(), BufferUsage::CopySource)) {
            device_.ReportValidationError("Copy source lacks the CopySource usage flag");
        }
        if (!HasAnyFlag(dst.Usage(), BufferUsage::CopyDestination)) {
            device_.ReportValidationError("Copy destination lacks the CopyDestination usage flag");
        }
        if (srcOffset + size > src.Size() || dstOffset + size > dst.Size()) {
            device_.ReportValidationError("CopyBuffer range exceeds a buffer");
        }
        if (insideRenderPass_) {
            device_.ReportValidationError("CopyBuffer inside a render pass");
        }
        ++stats_.copies;
    }

    void CopyBufferToTexture(const IBuffer& src, u64 srcOffset, u64 rowPitch, ITexture& dst,
                             const TextureCopyRegion& region) override {
        if (!CheckBegun()) {
            return;
        }
        if (!HasAnyFlag(src.Usage(), BufferUsage::CopySource)) {
            device_.ReportValidationError("Upload source lacks the CopySource usage flag");
        }
        if (!HasAnyFlag(dst.Desc().usage, TextureUsage::CopyDestination)) {
            device_.ReportValidationError("Upload target lacks the CopyDestination texture usage");
        }
        if (region.mipLevel >= dst.Desc().mipLevels) {
            device_.ReportValidationError("CopyBufferToTexture targets a missing mip level");
        }
        if (region.width == 0 || region.height == 0) {
            device_.ReportValidationError("CopyBufferToTexture with an empty region");
        }
        const u64 expected = ComputeMipBytes(dst.GetFormat(), region.width, region.height,
                                             region.mipLevel);
        if (srcOffset + expected > src.Size()) {
            device_.ReportValidationError("CopyBufferToTexture reads past the end of the buffer");
        }
        if (rowPitch > 0 && rowPitch < region.width * GetFormatInfo(dst.GetFormat()).bytesPerBlock) {
            device_.ReportValidationError("CopyBufferToTexture row pitch is smaller than the row");
        }
        ++stats_.copies;
    }

    void GenerateMipmaps(ITexture& texture) override {
        if (!CheckBegun()) {
            return;
        }
        if (texture.Desc().mipLevels < 2) {
            device_.ReportValidationError("GenerateMipmaps on a texture with a single mip level");
        }
        if (!HasAnyFlag(texture.Desc().usage, TextureUsage::Sampled)) {
            device_.ReportValidationError("GenerateMipmaps requires the Sampled texture usage");
        }
    }

    void WriteTimestamp(const IQueryPool& pool, u32 queryIndex) override {
        if (!CheckBegun()) {
            return;
        }
        auto& null = As<NullQueryPool>(pool);
        if (queryIndex >= null.QueryCount()) {
            device_.ReportValidationError("Timestamp query index out of range");
            return;
        }
        null.Write(queryIndex, Clock::NowNs());
        ++stats_.timestamps;
    }

    void BeginDebugLabel(std::string_view name, math::Color color) override {
        if (!CheckBegun()) {
            return;
        }
        debugLabels_.emplace_back(name);
        ++debugLabelDepth_;
        L3D_UNUSED(color);
    }

    void EndDebugLabel() override {
        if (!CheckBegun()) {
            return;
        }
        if (debugLabelDepth_ == 0) {
            device_.ReportValidationError("EndDebugLabel without a matching BeginDebugLabel");
            return;
        }
        --debugLabelDepth_;
        if (!debugLabels_.empty()) {
            debugLabels_.pop_back();
        }
    }

    [[nodiscard]] const Stats& GetStats() const noexcept override { return stats_; }

    /// True while recording, i.e. between Begin() and End().
    [[nodiscard]] bool IsOpen() const noexcept { return begun_; }
    /// True when a complete, error free recording is waiting to be submitted.
    [[nodiscard]] bool HasFinishedRecording() const noexcept { return finished_; }
    /// Consumes the recording; a command buffer must be recorded again to be
    /// submitted a second time (matches explicit APIs, where re-recording resets
    /// the buffer).
    void ConsumeRecording() noexcept { finished_ = false; }

private:
    [[nodiscard]] bool CheckBegun() {
        if (!begun_) {
            device_.ReportValidationError("Command recorded outside Begin/End");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool CheckDrawPreconditions(const char* what) {
        if (!CheckBegun()) {
            return false;
        }
        if (!insideRenderPass_) {
            device_.ReportValidationError(fmt::Format("{} outside a render pass", what));
            return false;
        }
        if (boundPipeline_ == nullptr) {
            device_.ReportValidationError(fmt::Format("{} without a pipeline bound", what));
            return false;
        }
        if (boundPipeline_->IsCompute()) {
            device_.ReportValidationError(fmt::Format("{} with a compute pipeline bound", what));
            return false;
        }
        if (!viewportSet_) {
            device_.ReportValidationError(fmt::Format("{} before SetViewport", what));
            return false;
        }
        return true;
    }

    void ValidateVertexBindings() {
        if (boundPipeline_ == nullptr) {
            return;
        }
        for (const VertexBinding& binding : boundPipeline_->Desc().vertexBindings) {
            const bool found = std::find(boundVertexBuffers_.begin(), boundVertexBuffers_.end(),
                                         binding.binding) != boundVertexBuffers_.end();
            if (!found) {
                device_.ReportValidationError(
                    fmt::Format("Vertex binding {} is not bound before drawing", binding.binding));
            }
        }
    }

    Stats stats_{};
    bool begun_ = false;
    bool finished_ = false;
    bool insideRenderPass_ = false;
    bool indexBufferBound_ = false;
    bool indexBuffer32Bit_ = false;
    bool viewportSet_ = false;
    u32 debugLabelDepth_ = 0;
    Viewport viewport_{};
    Rect2D scissor_{};
    const NullRenderPass* activePass_ = nullptr;
    const NullPipeline* boundPipeline_ = nullptr;
    std::vector<u32> boundVertexBuffers_;
    std::unordered_map<u32, u64> boundDescriptorSets_;
    std::vector<std::string> debugLabels_;
};

} // namespace

// --- NullDevice ------------------------------------------------------------

NullDevice::NullDevice(const DeviceDesc& desc) : desc_(desc) {
    frameCount_ = desc.frameCount > 0 ? desc.frameCount : 2;
    info_.backend = BackendType::Null;
    info_.deviceName = "Local3D Null Device";
    info_.apiVersion = "1.0";
    info_.validationEnabled = desc.enableValidation;
}

NullDevice::~NullDevice() {
    // Free everything still deferred so the leak report is accurate.
    for (DeferredEntry& entry : deferred_) {
        delete entry.resource;
    }
    deferred_.clear();
}

void NullDevice::ReportValidationError(std::string message) {
    if (!desc_.enableValidation) {
        return;
    }
    L3D_LOG_ERROR(LogCategory::Rhi, "RHI validation: {}", message);
    std::lock_guard<std::mutex> lock(validationMutex_);
    validationErrors_.push_back(std::move(message));
}

std::vector<std::string> NullDevice::ValidationErrors() const {
    std::lock_guard<std::mutex> lock(validationMutex_);
    return validationErrors_;
}

void NullDevice::ClearValidationErrors() {
    std::lock_guard<std::mutex> lock(validationMutex_);
    validationErrors_.clear();
}

void NullDevice::TrackAllocation(u64 bytes, bool isTexture) {
    if (isTexture) {
        textureBytes_ += bytes;
        textureCount_++;
    } else {
        bufferBytes_ += bytes;
        bufferCount_++;
    }
    liveResources_++;
    totalAllocatedBytes_ += bytes;
}

void NullDevice::UntrackResource(bool isTexture) {
    if (isTexture) {
        if (textureCount_ > 0) {
            textureCount_--;
        }
    } else if (bufferCount_ > 0) {
        bufferCount_--;
    }
    if (liveResources_ > 0) {
        liveResources_--;
    }
}

void NullDevice::TrackFree(u64 bytes) { totalFreedBytes_ += bytes; }

Result<BufferPtr> NullDevice::CreateBuffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Buffer size must be non-zero"});
    }
    if (desc.usage == BufferUsage::None) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Buffer usage must be specified"});
    }
    if (desc.size > info_.maxBufferSize) {
        return Unexpected(Status{StatusCode::OutOfRange, "Buffer exceeds the device size limit"});
    }
    auto buffer = std::unique_ptr<NullBuffer>(new NullBuffer(*this, desc, nextResourceId_++));
    TrackAllocation(desc.size, false);
    return BufferPtr(buffer.release());
}

Result<TexturePtr> NullDevice::CreateTexture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Texture dimensions must be non-zero"});
    }
    if (desc.format == Format::Unknown) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Texture format must be specified"});
    }
    if (desc.usage == TextureUsage::None) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Texture usage must be specified"});
    }
    if (desc.width > info_.maxTextureSize2D || desc.height > info_.maxTextureSize2D) {
        return Unexpected(Status{StatusCode::OutOfRange, "Texture exceeds the device size limit"});
    }
    if (desc.mipLevels == 0 || desc.mipLevels > MipLevelsFor(desc.width, desc.height)) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Invalid mip level count"});
    }
    if (desc.dimension == TextureDimension::Cube && (desc.arrayLayers % 6) != 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Cube textures need 6 layers per face"});
    }
    const FormatInfo& formatInfo = GetFormatInfo(desc.format);
    if (formatInfo.isCompressed && !info_.supportsBcTextures) {
        return Unexpected(Status{StatusCode::Unsupported, "Compressed textures are unsupported"});
    }

    auto texture = std::unique_ptr<NullTexture>(new NullTexture(*this, desc, nextResourceId_++));
    u64 bytes = 0;
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        bytes += ComputeMipBytes(desc.format, desc.width, desc.height, mip) * desc.arrayLayers;
    }
    texture->SetSizeInBytes(bytes);
    TrackAllocation(bytes, true);
    return TexturePtr(texture.release());
}

Result<SamplerPtr> NullDevice::CreateSampler(const SamplerDesc& desc) {
    if (desc.maxAnisotropy > 1.0f && !info_.supportsAnisotropicFiltering) {
        return Unexpected(Status{StatusCode::Unsupported, "Anisotropic filtering is unsupported"});
    }
    auto sampler = std::unique_ptr<NullSampler>(new NullSampler(*this, desc, nextResourceId_++));
    return SamplerPtr(sampler.release());
}

Result<ShaderModulePtr> NullDevice::CreateShaderModule(const ShaderModuleDesc& desc) {
    if (desc.bytecode.empty()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Shader bytecode is empty"});
    }
    if (desc.entryPoint.empty()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Shader entry point is empty"});
    }
    if (desc.stage == ShaderStage::Compute && !info_.supportsCompute) {
        return Unexpected(Status{StatusCode::Unsupported, "Compute shaders are unsupported"});
    }
    auto module =
        std::unique_ptr<NullShaderModule>(new NullShaderModule(*this, desc, nextResourceId_++));
    return ShaderModulePtr(module.release());
}

Result<DescriptorSetLayoutPtr>
NullDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    if (desc.bindings.Empty()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Descriptor layout has no bindings"});
    }
    auto layout = std::unique_ptr<NullDescriptorSetLayout>(
        new NullDescriptorSetLayout(*this, desc, nextResourceId_++));
    return DescriptorSetLayoutPtr(layout.release());
}

Result<DescriptorSetPtr> NullDevice::AllocateDescriptorSet(const IDescriptorSetLayout& layout) {
    const auto& null = As<NullDescriptorSetLayout>(layout);
    auto set = std::unique_ptr<NullDescriptorSet>(
        new NullDescriptorSet(*this, null, nextResourceId_++));
    return DescriptorSetPtr(set.release());
}

void NullDevice::WriteDescriptorSet(IDescriptorSet& set, std::span<const DescriptorWrite> writes) {
    auto& null = As<NullDescriptorSet>(set);
    const auto& bindings = null.Layout().Desc().bindings;
    for (const DescriptorWrite& write : writes) {
        bool found = false;
        for (const DescriptorBinding& binding : bindings) {
            if (binding.binding == write.binding) {
                found = true;
                if (binding.type != write.type) {
                    ReportValidationError("Descriptor write type does not match the layout");
                }
                if (write.arrayIndex >= binding.count) {
                    ReportValidationError("Descriptor array index out of range");
                }
            }
        }
        if (!found) {
            ReportValidationError(fmt::Format("Descriptor write to undeclared binding {}",
                                              write.binding));
        }
        if (write.buffer != nullptr && write.bufferOffset + write.bufferRange > write.buffer->Size() &&
            write.bufferRange != 0) {
            ReportValidationError("Descriptor buffer range exceeds the buffer");
        }
        if (write.texture != nullptr &&
            write.baseMipLevel + write.mipLevelCount > write.texture->Desc().mipLevels) {
            ReportValidationError("Descriptor mip range exceeds the texture");
        }
        null.RecordWrite(write);
    }
}

Result<RenderPassPtr> NullDevice::CreateRenderPass(const RenderPassDesc& desc) {
    if (desc.colorAttachments.Empty() && !desc.hasDepthStencil) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Render pass has no attachments"});
    }
    if (desc.colorAttachments.Size() > info_.maxColorAttachments) {
        return Unexpected(Status{StatusCode::OutOfRange, "Too many color attachments"});
    }
    auto pass = std::unique_ptr<NullRenderPass>(new NullRenderPass(*this, desc, nextResourceId_++));
    return RenderPassPtr(pass.release());
}

Result<FramebufferPtr> NullDevice::CreateFramebuffer(const IRenderPass& renderPass,
                                                     std::span<const ITexture* const> attachments,
                                                     u32 width, u32 height,
                                                     std::string debugName) {
    const auto& pass = As<NullRenderPass>(renderPass);
    const usize expected = pass.Desc().colorAttachments.Size() + (pass.Desc().hasDepthStencil ? 1 : 0);
    if (attachments.size() != expected) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Attachment count mismatch"});
    }
    for (const ITexture* texture : attachments) {
        if (texture == nullptr) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Null framebuffer attachment"});
        }
        if (texture->Width() != width || texture->Height() != height) {
            return Unexpected(
                Status{StatusCode::InvalidArgument, "Framebuffer attachment size mismatch"});
        }
    }
    auto framebuffer = std::unique_ptr<NullFramebuffer>(new NullFramebuffer(
        *this, pass, attachments, width, height, std::move(debugName), nextResourceId_++));
    return FramebufferPtr(framebuffer.release());
}

namespace {

[[nodiscard]] bool ValidatePipeline(NullDevice& device, const PipelineDesc& desc, bool compute) {
    bool valid = true;
    if (desc.shaders.Empty()) {
        device.ReportValidationError("Pipeline has no shaders");
        valid = false;
    }
    bool hasVertex = false;
    bool hasFragment = false;
    bool hasCompute = false;
    for (const ShaderModuleDesc& shader : desc.shaders) {
        switch (shader.stage) {
            case ShaderStage::Vertex: hasVertex = true; break;
            case ShaderStage::Fragment: hasFragment = true; break;
            case ShaderStage::Compute: hasCompute = true; break;
        }
        if (shader.bytecode.empty()) {
            device.ReportValidationError("Pipeline shader has empty bytecode");
            valid = false;
        }
    }
    if (compute) {
        if (!hasCompute) {
            device.ReportValidationError("Compute pipeline without a compute shader");
            valid = false;
        }
    } else {
        if (!hasVertex || !hasFragment) {
            device.ReportValidationError("Graphics pipeline needs a vertex and a fragment shader");
            valid = false;
        }
        if (hasCompute) {
            device.ReportValidationError("Graphics pipeline cannot contain a compute shader");
            valid = false;
        }
        // A depth-only pipeline (shadow maps) legitimately has no color formats,
        // which explicit APIs allow; what is never valid is declaring neither.
        if (desc.colorFormats.Empty() && !(desc.hasDepthAttachment &&
                                           IsDepthFormat(desc.depthFormat))) {
            device.ReportValidationError(
                "Graphics pipeline declares neither color nor depth attachments");
            valid = false;
        }
    }
    for (const PushConstantRange& range : desc.pushConstantRanges) {
        if (range.offset + range.size > device.Info().maxPushConstantSize) {
            device.ReportValidationError("Push constant range exceeds the device limit");
            valid = false;
        }
    }
    for (const VertexBinding& binding : desc.vertexBindings) {
        if (binding.stride == 0) {
            device.ReportValidationError("Vertex binding has a zero stride");
            valid = false;
        }
    }
    for (const VertexAttribute& attribute : desc.vertexAttributes) {
        bool bindingFound = false;
        for (const VertexBinding& binding : desc.vertexBindings) {
            if (binding.binding == attribute.binding) {
                bindingFound = true;
            }
        }
        if (!bindingFound) {
            device.ReportValidationError("Vertex attribute references an unknown binding");
            valid = false;
        }
    }
    return valid;
}

} // namespace

Result<PipelinePtr> NullDevice::CreateGraphicsPipeline(const PipelineDesc& desc) {
    // Validation records errors on the device; the pipeline is still created so
    // callers can keep going (the Vulkan backend would return an error here).
    const bool valid = ValidatePipeline(*this, desc, false);
    L3D_UNUSED(valid);
    auto pipeline = std::unique_ptr<NullPipeline>(new NullPipeline(*this, desc, false, nextResourceId_++));
    return PipelinePtr(pipeline.release());
}

Result<PipelinePtr> NullDevice::CreateComputePipeline(const PipelineDesc& desc) {
    if (!info_.supportsCompute) {
        return Unexpected(Status{StatusCode::Unsupported, "Compute is not supported"});
    }
    const bool valid = ValidatePipeline(*this, desc, true);
    L3D_UNUSED(valid);
    auto pipeline = std::unique_ptr<NullPipeline>(new NullPipeline(*this, desc, true, nextResourceId_++));
    return PipelinePtr(pipeline.release());
}

Result<CommandBufferPtr> NullDevice::CreateCommandBuffer() {
    auto buffer = std::unique_ptr<NullCommandBuffer>(new NullCommandBuffer(*this, nextResourceId_++));
    return CommandBufferPtr(buffer.release());
}

Result<QueryPoolPtr> NullDevice::CreateTimestampQueryPool(u32 queryCount) {
    if (queryCount == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Query pool must have at least one query"});
    }
    if (!info_.supportsTimestampQueries) {
        return Unexpected(Status{StatusCode::Unsupported, "Timestamp queries are unsupported"});
    }
    auto pool = std::unique_ptr<NullQueryPool>(new NullQueryPool(*this, queryCount, nextResourceId_++));
    return QueryPoolPtr(pool.release());
}

Result<SwapchainPtr> NullDevice::CreateSwapchain(const SwapchainDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Swapchain extent must be non-zero"});
    }
    auto swapchain = std::unique_ptr<NullSwapchain>(new NullSwapchain(*this, desc, nextResourceId_++));
    return SwapchainPtr(swapchain.release());
}

void NullDevice::UpdateBuffer(IBuffer& buffer, u64 offset, ConstByteSpan data) {
    auto& null = As<NullBuffer>(buffer);
    if (offset + data.size() > buffer.Size()) {
        ReportValidationError("UpdateBuffer writes past the end of the buffer");
        return;
    }
    if (data.empty()) {
        return;
    }
    std::memcpy(null.Storage().data() + offset, data.data(), data.size());
}

void NullDevice::UpdateTexture(ITexture& texture, std::span<const ConstByteSpan> mipData) {
    auto& null = As<NullTexture>(texture);
    if (mipData.size() != texture.Desc().mipLevels) {
        ReportValidationError("UpdateTexture mip count does not match the texture");
        return;
    }
    std::vector<std::vector<std::byte>> stored;
    stored.reserve(mipData.size());
    for (usize mip = 0; mip < mipData.size(); ++mip) {
        const u64 expected = ComputeMipBytes(texture.GetFormat(), texture.Width(), texture.Height(),
                                             static_cast<u32>(mip));
        if (mipData[mip].size() < expected) {
            ReportValidationError(fmt::Format("UpdateTexture mip {} is smaller than expected", mip));
            return;
        }
        stored.emplace_back(mipData[mip].begin(),
                            mipData[mip].begin() + static_cast<isize>(expected));
    }
    null.SetMipData(std::move(stored));
}

void NullDevice::Submit(ICommandBuffer& commandBuffer) {
    auto& null = As<NullCommandBuffer>(commandBuffer);
    if (null.IsOpen()) {
        ReportValidationError("Submitted a command buffer that is still recording (missing End)");
        return;
    }
    if (!null.HasFinishedRecording()) {
        ReportValidationError(
            "Submitted a command buffer without a finished recording (never begun, already "
            "submitted, or ended with errors)");
        return;
    }
    null.ConsumeRecording();
    ++submittedCommandBuffers_;
}

void NullDevice::BeginFrame() {
    ProcessDeferredDeletions();
    frameIndex_ = (frameIndex_ + 1) % frameCount_;
}

void NullDevice::EndFrame() { ++frameNumber_; }

void NullDevice::WaitIdle() {
    // Nothing is ever in flight in the null backend, so free everything now.
    const u64 previousFrame = frameNumber_;
    frameNumber_ += frameCount_ + 1;
    ProcessDeferredDeletions();
    frameNumber_ = previousFrame;
}

std::vector<f64> NullDevice::ReadTimestamps(const IQueryPool& pool, u32 firstQuery, u32 count) {
    const auto& null = As<NullQueryPool>(pool);
    std::vector<f64> results;
    if (firstQuery + count > null.QueryCount()) {
        ReportValidationError("ReadTimestamps range exceeds the query pool");
        return results;
    }
    results.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        results.push_back(static_cast<f64>(null.Read(firstQuery + i)));
    }
    return results;
}

void NullDevice::ScheduleRelease(GpuResource* resource) {
    if (resource == nullptr) {
        return;
    }
    deferred_.push_back(DeferredEntry{resource, frameNumber_});
}

void NullDevice::ProcessDeferredDeletions() {
    // A resource released during frame N may still be read by frames N+1 ..
    // N+frameCount-1, so it survives until frameNumber has advanced past them.
    std::vector<DeferredEntry> ready;
    std::vector<DeferredEntry> pending;
    ready.reserve(deferred_.size());
    pending.reserve(deferred_.size());
    for (const DeferredEntry& entry : deferred_) {
        if (frameNumber_ - entry.releaseFrame >= frameCount_) {
            ready.push_back(entry);
        } else {
            pending.push_back(entry);
        }
    }
    deferred_.swap(pending);
    for (DeferredEntry& entry : ready) {
        const bool isTexture = dynamic_cast<ITexture*>(entry.resource) != nullptr;
        if (const auto* buffer = dynamic_cast<const IBuffer*>(entry.resource)) {
            TrackFree(buffer->Size());
        } else if (const auto* texture = dynamic_cast<const ITexture*>(entry.resource)) {
            TrackFree(texture->SizeInBytes());
        }
        UntrackResource(isTexture);
        delete entry.resource;
    }
}

IDevice::MemoryReport NullDevice::MemoryUsage() const {
    MemoryReport report;
    report.bufferBytes = bufferBytes_;
    report.textureBytes = textureBytes_;
    report.bufferCount = bufferCount_;
    report.textureCount = textureCount_;
    report.liveResources = liveResources_;
    report.deferredResources = static_cast<u32>(deferred_.size());
    report.totalAllocatedBytes = totalAllocatedBytes_;
    report.totalFreedBytes = totalFreedBytes_;
    return report;
}

} // namespace l3d::rhi::null
// NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
