#pragma once
/// @file RenderGraph.hpp
/// @brief A frame graph: declare passes and resources, let the graph order them.
///
/// Why a graph instead of hand written render code?
///   * Ordering is derived from data dependencies, so inserting a pass (SSAO,
///     TAA history, a debug view) cannot silently break the frame.
///   * Passes whose results nothing consumes are removed before any GPU work is
///     recorded - the editor can enable five debug views and pay for one.
///   * Transient resources are allocated by the graph, so their lifetime is
///     explicit and their memory is reported in one place.
///
/// Lifetime model: a RenderGraph object is built and compiled once per frame
/// description, then executed.  It owns no RHI resources of its own except the
/// transient cache, which it releases in its destructor.

#include "local3d/core/Result.hpp"
#include "local3d/containers/SmallVector.hpp"
#include "local3d/rendergraph/RenderGraphTypes.hpp"
#include "local3d/rhi/RhiDevice.hpp"
#include "local3d/rhi/RhiResources.hpp"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace l3d::graph {

class PassContext;

/// Records commands for one pass.  Captured by the declaring code, so it can
/// close over whatever the pass needs (material lists, culling results).
using PassExecuteFn = std::function<void(PassContext&)>;

/// Handed to the caller while a pass is being declared.  Every resource a pass
/// touches must be declared here: the graph derives ordering from it and
/// Execute() rejects accesses that were not declared.
class PassBuilder {
public:
    explicit PassBuilder(std::vector<ResourceAccess>& accesses) : accesses_(accesses) {}

    void Read(TextureHandle texture);
    void Write(TextureHandle texture);
    void ReadWrite(TextureHandle texture);
    void Read(BufferHandle buffer);
    void Write(BufferHandle buffer);
    void ReadWrite(BufferHandle buffer);

    [[nodiscard]] const std::vector<ResourceAccess>& Accesses() const noexcept {
        return accesses_;
    }

private:
    std::vector<ResourceAccess>& accesses_;
};

/// Handed to a pass while it records.  Access is limited to the resources the
/// pass declared, which keeps the graph's dependency information honest.
class PassContext {
public:
    PassContext(rhi::IDevice& device, rhi::ICommandBuffer& commands, const PassInfo& info,
                std::span<const ResourceAccess> accesses)
        : device_(device), commands_(commands), info_(info), accesses_(accesses) {}

    [[nodiscard]] rhi::IDevice& Device() const noexcept { return device_; }
    [[nodiscard]] rhi::ICommandBuffer& Commands() const noexcept { return commands_; }
    [[nodiscard]] const std::string& PassName() const noexcept { return info_.name; }
    [[nodiscard]] PassKind Kind() const noexcept { return info_.kind; }

    /// Resolve a declared resource.  Returns nullptr when the handle was not
    /// declared by this pass or could not be allocated; the miss is recorded as
    /// a validation error rather than producing a dangling reference.
    [[nodiscard]] rhi::ITexture* Texture(TextureHandle handle) const;
    [[nodiscard]] rhi::IBuffer* Buffer(BufferHandle handle) const;
    [[nodiscard]] bool HasTexture(TextureHandle handle) const noexcept;
    [[nodiscard]] bool HasBuffer(BufferHandle handle) const noexcept;

    /// Number of accesses this pass did not declare (see above).
    [[nodiscard]] u32 UndeclaredAccesses() const noexcept { return undeclaredAccesses_; }

private:
    friend class RenderGraph;

    void BindTexture(u32 index, rhi::ITexture* texture) const;
    void BindBuffer(u32 index, rhi::IBuffer* buffer) const;
    void ReportUndeclared() const;

    rhi::IDevice& device_;
    rhi::ICommandBuffer& commands_;
    const PassInfo& info_;
    std::span<const ResourceAccess> accesses_;
    // Resolved resources for the current execution, indexed by resource index.
    mutable std::vector<rhi::ITexture*> textures_;
    mutable std::vector<rhi::IBuffer*> buffers_;
    mutable u32 undeclaredAccesses_ = 0;
};

/// The frame graph.  Not thread safe: build, compile and execute on one thread
/// (the render thread), which is also where the RHI is used.
class RenderGraph {
public:
    RenderGraph() = default;

    // --- Resource declaration --------------------------------------------
    /// A resource the graph allocates for the duration of the frame.
    [[nodiscard]] TextureHandle CreateTexture(const TextureSpec& spec);
    [[nodiscard]] BufferHandle CreateBuffer(const BufferSpec& spec);
    /// A resource owned elsewhere (swapchain image, persistent history buffer).
    [[nodiscard]] TextureHandle ImportTexture(rhi::ITexture& texture, std::string name);
    [[nodiscard]] BufferHandle ImportBuffer(rhi::IBuffer& buffer, std::string name);
    /// Keeps the resource, and every pass that produces it, alive.
    void MarkOutput(TextureHandle texture);
    void MarkOutput(BufferHandle buffer);

    // --- Pass declaration -------------------------------------------------
    /// `setup` declares the pass's reads and writes; `execute` records them.
    /// `setup` runs during Compile(), `execute` during Execute().
    /// The returned handle is optional: the pass is registered either way.
    PassHandle AddRasterPass(std::string name,
                                          std::function<void(PassBuilder&)> setup,
                                          PassExecuteFn execute);
    PassHandle AddComputePass(std::string name,
                                           std::function<void(PassBuilder&)> setup,
                                           PassExecuteFn execute);
    PassHandle AddCopyPass(std::string name, std::function<void(PassBuilder&)> setup,
                                         PassExecuteFn execute);
    /// A pass that must run even if nothing reads its output.
    void MarkSideEffects(PassHandle pass);

    // --- Compilation ------------------------------------------------------
    /// Runs the setup callbacks, builds the dependency graph, culls dead passes
    /// and produces the execution order.  Fails on unknown handles or cycles.
    [[nodiscard]] Result<void> Compile();

    [[nodiscard]] bool IsCompiled() const noexcept { return compiled_; }
    [[nodiscard]] std::span<const PassHandle> ExecutionOrder() const noexcept {
        return executionOrder_;
    }
    [[nodiscard]] const PassInfo& Pass(PassHandle pass) const;
    [[nodiscard]] const ResourceInfo& Texture(TextureHandle handle) const;
    [[nodiscard]] const ResourceInfo& Buffer(BufferHandle handle) const;
    [[nodiscard]] const CompileStats& Stats() const noexcept { return stats_; }
    /// Every access declared by a pass, for the frame debugger.
    [[nodiscard]] std::span<const ResourceAccess> PassAccesses(PassHandle pass) const;

    // --- Execution --------------------------------------------------------
    /// Allocates transient resources (reusing the cache when a description is
    /// unchanged) and records every surviving pass in order.
    [[nodiscard]] Result<void> Execute(rhi::IDevice& device, rhi::ICommandBuffer& commands);

    /// Number of validation errors recorded during Compile/Execute.
    [[nodiscard]] u32 ValidationErrorCount() const noexcept { return validationErrors_; }
    [[nodiscard]] const std::vector<std::string>& ValidationErrors() const noexcept {
        return validationErrorList_;
    }

    /// Drops every cached transient resource.  Call on resize or device loss.
    void ReleaseTransientResources();

    /// Forget all passes; resources stay valid so a graph can be rebuilt cheaply
    /// when only the pass list changes.
    void ClearPasses();

    /// Forget passes *and* resources, releasing every cached transient.  Invalid
    /// handles afterwards.  Use when the frame's resource set itself changes, for
    /// example on a resize, so the graph does not accumulate stale entries.
    void Reset();

private:
    struct TextureEntry {
        TextureSpec spec;
        rhi::ITexture* external = nullptr;
        rhi::TexturePtr owned;
        ResourceInfo info;
    };
    struct BufferEntry {
        BufferSpec spec;
        rhi::IBuffer* external = nullptr;
        rhi::BufferPtr owned;
        ResourceInfo info;
    };
    struct PassEntry {
        std::string name;
        PassKind kind = PassKind::Raster;
        std::function<void(PassBuilder&)> setup;
        PassExecuteFn execute;
        std::vector<ResourceAccess> accesses;
        PassInfo info;
        bool setupRan = false;
    };
    struct CacheKey {
        u32 width = 0;
        u32 height = 0;
        u32 mipLevels = 0;
        u32 arrayLayers = 0;
        rhi::Format format = rhi::Format::Unknown;
        rhi::TextureDimension dimension = rhi::TextureDimension::Tex2D;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        rhi::SampleCount samples = rhi::SampleCount::One;

        friend bool operator==(const CacheKey&, const CacheKey&) = default;
    };
    struct BufferCacheKey {
        u64 size = 0;
        rhi::BufferUsage usage = rhi::BufferUsage::None;

        friend bool operator==(const BufferCacheKey&, const BufferCacheKey&) = default;
    };

    [[nodiscard]] PassHandle AddPass(std::string name, PassKind kind,
                                     std::function<void(PassBuilder&)> setup,
                                     PassExecuteFn execute);
    void RunSetupCallbacks();
    [[nodiscard]] Result<void> BuildDependencies();
    void CullPasses();
    [[nodiscard]] Result<void> TopologicalSort();
    void ComputeLifetimes();
    void ReportValidationError(std::string message);
    [[nodiscard]] Result<rhi::ITexture*> ResolveTexture(u32 index, rhi::IDevice& device);
    [[nodiscard]] Result<rhi::IBuffer*> ResolveBuffer(u32 index, rhi::IDevice& device);
    [[nodiscard]] bool IsValidTexture(u32 index) const noexcept { return index < textures_.size(); }
    [[nodiscard]] bool IsValidBuffer(u32 index) const noexcept { return index < buffers_.size(); }

    std::vector<TextureEntry> textures_;
    std::vector<BufferEntry> buffers_;
    std::vector<PassEntry> passes_;
    std::vector<PassHandle> executionOrder_;
    /// Writer pass index per resource (kInvalidHandle when unwritten).
    std::vector<u32> textureWriters_;
    std::vector<u32> bufferWriters_;
    /// Adjacency: pass index -> pass indices that must run after it.
    std::vector<std::vector<u32>> successors_;
    std::vector<u32> incoming_;
    CompileStats stats_{};
    std::vector<std::string> validationErrorList_;
    u32 validationErrors_ = 0;
    bool compiled_ = false;

    // Transient resource cache, keyed by description.  Reusing allocations
    // across frames is what makes a per-frame graph cheap.
    std::vector<std::pair<CacheKey, rhi::TexturePtr>> textureCache_;
    std::vector<std::pair<BufferCacheKey, rhi::BufferPtr>> bufferCache_;
};

} // namespace l3d::graph
