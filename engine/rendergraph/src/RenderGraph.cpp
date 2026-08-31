#include "local3d/rendergraph/RenderGraph.hpp"

#include "local3d/core/Assert.hpp"
#include "local3d/core/Enum.hpp"
#include "local3d/core/Log.hpp"
#include "local3d/rhi/RhiTypes.hpp"

#include <algorithm>
#include <utility>

namespace l3d::graph {
namespace {

[[nodiscard]] u64 TextureSizeBytes(const TextureSpec& spec) {
    u64 bytes = 0;
    for (u32 mip = 0; mip < spec.mipLevels; ++mip) {
        bytes += rhi::ComputeMipBytes(spec.format, spec.width, spec.height, mip) * spec.depth *
                 spec.arrayLayers;
    }
    return bytes;
}

} // namespace

rhi::TextureDesc ToRhiDesc(const TextureSpec& spec) {
    rhi::TextureDesc desc;
    desc.width = spec.width;
    desc.height = spec.height;
    desc.depth = spec.depth;
    desc.mipLevels = spec.mipLevels;
    desc.arrayLayers = spec.arrayLayers;
    desc.format = spec.format;
    desc.dimension = spec.dimension;
    desc.usage = spec.usage;
    desc.samples = spec.samples;
    desc.debugName = spec.name;
    return desc;
}

rhi::BufferDesc ToRhiDesc(const BufferSpec& spec) {
    rhi::BufferDesc desc;
    desc.size = spec.size;
    desc.usage = spec.usage;
    desc.debugName = spec.name;
    return desc;
}

std::string_view PassKindName(PassKind kind) noexcept {
    switch (kind) {
        case PassKind::Raster: return "raster";
        case PassKind::Compute: return "compute";
        case PassKind::Copy: return "copy";
    }
    return "unknown";
}

// --- PassBuilder -----------------------------------------------------------

void PassBuilder::Read(TextureHandle texture) {
    accesses_.push_back(ResourceAccess{texture.Index(), true, AccessKind::Read});
}
void PassBuilder::Write(TextureHandle texture) {
    accesses_.push_back(ResourceAccess{texture.Index(), true, AccessKind::Write});
}
void PassBuilder::ReadWrite(TextureHandle texture) {
    accesses_.push_back(ResourceAccess{texture.Index(), true, AccessKind::ReadWrite});
}
void PassBuilder::Read(BufferHandle buffer) {
    accesses_.push_back(ResourceAccess{buffer.Index(), false, AccessKind::Read});
}
void PassBuilder::Write(BufferHandle buffer) {
    accesses_.push_back(ResourceAccess{buffer.Index(), false, AccessKind::Write});
}
void PassBuilder::ReadWrite(BufferHandle buffer) {
    accesses_.push_back(ResourceAccess{buffer.Index(), false, AccessKind::ReadWrite});
}

// --- PassContext -----------------------------------------------------------

bool PassContext::HasTexture(TextureHandle handle) const noexcept {
    return std::any_of(accesses_.begin(), accesses_.end(), [&](const ResourceAccess& access) {
        return access.isTexture && access.resource == handle.Index();
    });
}

bool PassContext::HasBuffer(BufferHandle handle) const noexcept {
    return std::any_of(accesses_.begin(), accesses_.end(), [&](const ResourceAccess& access) {
        return !access.isTexture && access.resource == handle.Index();
    });
}

void PassContext::ReportUndeclared() const {
    ++undeclaredAccesses_;
    L3D_LOG_ERROR(LogCategory::RenderGraph,
                  "Pass '{}' accessed a resource it did not declare in its setup callback",
                  info_.name);
}

rhi::ITexture* PassContext::Texture(TextureHandle handle) const {
    if (!HasTexture(handle)) {
        ReportUndeclared();
        return nullptr;
    }
    if (handle.Index() >= textures_.size()) {
        return nullptr;
    }
    return textures_[handle.Index()];
}

rhi::IBuffer* PassContext::Buffer(BufferHandle handle) const {
    if (!HasBuffer(handle)) {
        ReportUndeclared();
        return nullptr;
    }
    if (handle.Index() >= buffers_.size()) {
        return nullptr;
    }
    return buffers_[handle.Index()];
}

void PassContext::BindTexture(u32 index, rhi::ITexture* texture) const {
    if (textures_.size() <= index) {
        textures_.resize(index + 1, nullptr);
    }
    textures_[index] = texture;
}

void PassContext::BindBuffer(u32 index, rhi::IBuffer* buffer) const {
    if (buffers_.size() <= index) {
        buffers_.resize(index + 1, nullptr);
    }
    buffers_[index] = buffer;
}

// --- Resource declaration --------------------------------------------------

TextureHandle RenderGraph::CreateTexture(const TextureSpec& spec) {
    TextureEntry entry;
    entry.spec = spec;
    entry.info.name = spec.name;
    entry.info.isTexture = true;
    entry.info.isExternal = false;
    entry.info.sizeBytes = TextureSizeBytes(spec);
    textures_.push_back(std::move(entry));
    textureWriters_.push_back(kInvalidHandle);
    compiled_ = false;
    return TextureHandle{static_cast<u32>(textures_.size() - 1)};
}

BufferHandle RenderGraph::CreateBuffer(const BufferSpec& spec) {
    BufferEntry entry;
    entry.spec = spec;
    entry.info.name = spec.name;
    entry.info.isTexture = false;
    entry.info.isExternal = false;
    entry.info.sizeBytes = spec.size;
    buffers_.push_back(std::move(entry));
    bufferWriters_.push_back(kInvalidHandle);
    compiled_ = false;
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

TextureHandle RenderGraph::ImportTexture(rhi::ITexture& texture, std::string name) {
    TextureEntry entry;
    entry.spec.name = name;
    entry.spec.width = texture.Width();
    entry.spec.height = texture.Height();
    entry.spec.mipLevels = texture.Desc().mipLevels;
    entry.spec.arrayLayers = texture.Desc().arrayLayers;
    entry.spec.format = texture.GetFormat();
    entry.spec.dimension = texture.Desc().dimension;
    entry.spec.usage = texture.Desc().usage;
    entry.spec.samples = texture.Desc().samples;
    entry.external = &texture;
    entry.info.name = std::move(name);
    entry.info.isTexture = true;
    entry.info.isExternal = true;
    entry.info.sizeBytes = texture.SizeInBytes();
    textures_.push_back(std::move(entry));
    textureWriters_.push_back(kInvalidHandle);
    compiled_ = false;
    return TextureHandle{static_cast<u32>(textures_.size() - 1)};
}

BufferHandle RenderGraph::ImportBuffer(rhi::IBuffer& buffer, std::string name) {
    BufferEntry entry;
    entry.spec.name = name;
    entry.spec.size = buffer.Size();
    entry.spec.usage = buffer.Usage();
    entry.external = &buffer;
    entry.info.name = std::move(name);
    entry.info.isTexture = false;
    entry.info.isExternal = true;
    entry.info.sizeBytes = buffer.Size();
    buffers_.push_back(std::move(entry));
    bufferWriters_.push_back(kInvalidHandle);
    compiled_ = false;
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

void RenderGraph::MarkOutput(TextureHandle texture) {
    if (IsValidTexture(texture.Index())) {
        textures_[texture.Index()].info.isOutput = true;
        compiled_ = false;
    }
}

void RenderGraph::MarkOutput(BufferHandle buffer) {
    if (IsValidBuffer(buffer.Index())) {
        buffers_[buffer.Index()].info.isOutput = true;
        compiled_ = false;
    }
}

// --- Pass declaration ------------------------------------------------------

PassHandle RenderGraph::AddPass(std::string name, PassKind kind,
                                std::function<void(PassBuilder&)> setup, PassExecuteFn execute) {
    PassEntry entry;
    entry.name = std::move(name);
    entry.kind = kind;
    entry.setup = std::move(setup);
    entry.execute = std::move(execute);
    entry.info.name = entry.name;
    entry.info.kind = kind;
    passes_.push_back(std::move(entry));
    compiled_ = false;
    return PassHandle{static_cast<u32>(passes_.size() - 1)};
}

PassHandle RenderGraph::AddRasterPass(std::string name, std::function<void(PassBuilder&)> setup,
                                      PassExecuteFn execute) {
    return AddPass(std::move(name), PassKind::Raster, std::move(setup), std::move(execute));
}

PassHandle RenderGraph::AddComputePass(std::string name, std::function<void(PassBuilder&)> setup,
                                       PassExecuteFn execute) {
    return AddPass(std::move(name), PassKind::Compute, std::move(setup), std::move(execute));
}

PassHandle RenderGraph::AddCopyPass(std::string name, std::function<void(PassBuilder&)> setup,
                                    PassExecuteFn execute) {
    return AddPass(std::move(name), PassKind::Copy, std::move(setup), std::move(execute));
}

void RenderGraph::MarkSideEffects(PassHandle pass) {
    if (pass.Index() < passes_.size()) {
        passes_[pass.Index()].info.hasSideEffects = true;
        compiled_ = false;
    }
}

void RenderGraph::ClearPasses() {
    passes_.clear();
    executionOrder_.clear();
    // The statistics describe the last compile; without passes they are wrong,
    // so they go back to zero rather than reporting a stale frame.
    stats_ = CompileStats{};
    compiled_ = false;
    for (TextureEntry& entry : textures_) {
        entry.info.firstUse = kInvalidHandle;
        entry.info.lastUse = 0;
        entry.info.writerCount = 0;
        entry.info.readerCount = 0;
    }
    for (BufferEntry& entry : buffers_) {
        entry.info.firstUse = kInvalidHandle;
        entry.info.lastUse = 0;
        entry.info.writerCount = 0;
        entry.info.readerCount = 0;
    }
}

void RenderGraph::Reset() {
    ClearPasses();
    ReleaseTransientResources();
    textures_.clear();
    buffers_.clear();
    textureWriters_.clear();
    bufferWriters_.clear();
    stats_ = CompileStats{};
    compiled_ = false;
}

// --- Compilation -----------------------------------------------------------

void RenderGraph::RunSetupCallbacks() {
    const usize passCountBefore = passes_.size();
    for (PassEntry& entry : passes_) {
        entry.accesses.clear();
        entry.setupRan = false;
        if (!entry.setup) {
            entry.setupRan = true;
            continue;
        }
        PassBuilder builder(entry.accesses);
        entry.setup(builder);
        entry.setupRan = true;
    }
    if (passes_.size() != passCountBefore) {
        ReportValidationError("A pass setup callback added passes to the graph");
    }
}

Result<void> RenderGraph::BuildDependencies() {
    // Every resource has exactly one writer.  That single invariant is what
    // makes the graph independent of declaration order: a reader always knows
    // which pass produces its input, so a pass may be declared before the pass
    // it depends on.  Two writers would be a race, so it is an error.
    std::fill(textureWriters_.begin(), textureWriters_.end(), kInvalidHandle);
    std::fill(bufferWriters_.begin(), bufferWriters_.end(), kInvalidHandle);
    std::vector<std::vector<u32>> textureReaders(textures_.size());
    std::vector<std::vector<u32>> bufferReaders(buffers_.size());

    for (TextureEntry& entry : textures_) {
        entry.info.writerCount = 0;
        entry.info.readerCount = 0;
    }
    for (BufferEntry& entry : buffers_) {
        entry.info.writerCount = 0;
        entry.info.readerCount = 0;
    }

    for (u32 passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        const PassEntry& entry = passes_[passIndex];
        bool writesRenderTarget = false;
        for (const ResourceAccess& access : entry.accesses) {
            const bool reads = access.kind != AccessKind::Write;
            const bool writes = access.kind != AccessKind::Read;
            if (access.isTexture) {
                if (!IsValidTexture(access.resource)) {
                    return Unexpected(Status{
                        StatusCode::InvalidArgument,
                        "Pass '" + entry.name + "' references an unknown texture handle"});
                }
                TextureEntry& resource = textures_[access.resource];
                if (reads) {
                    ++resource.info.readerCount;
                    textureReaders[access.resource].push_back(passIndex);
                }
                if (writes) {
                    ++resource.info.writerCount;
                    if (HasAnyFlag(resource.spec.usage, rhi::TextureUsage::ColorAttachment) ||
                        HasAnyFlag(resource.spec.usage,
                                   rhi::TextureUsage::DepthStencilAttachment)) {
                        writesRenderTarget = true;
                    }
                    const u32 existing = textureWriters_[access.resource];
                    if (existing != kInvalidHandle && existing != passIndex) {
                        return Unexpected(
                            Status{StatusCode::InvalidState,
                                   "Resource '" + resource.spec.name + "' is written by both '" +
                                       passes_[existing].name + "' and '" + entry.name + "'"});
                    }
                    textureWriters_[access.resource] = passIndex;
                }
            } else {
                if (!IsValidBuffer(access.resource)) {
                    return Unexpected(Status{
                        StatusCode::InvalidArgument,
                        "Pass '" + entry.name + "' references an unknown buffer handle"});
                }
                BufferEntry& resource = buffers_[access.resource];
                if (reads) {
                    ++resource.info.readerCount;
                    bufferReaders[access.resource].push_back(passIndex);
                }
                if (writes) {
                    ++resource.info.writerCount;
                    const u32 existing = bufferWriters_[access.resource];
                    if (existing != kInvalidHandle && existing != passIndex) {
                        return Unexpected(
                            Status{StatusCode::InvalidState,
                                   "Resource '" + resource.spec.name + "' is written by both '" +
                                       passes_[existing].name + "' and '" + entry.name + "'"});
                    }
                    bufferWriters_[access.resource] = passIndex;
                }
            }
        }

        if (entry.kind == PassKind::Raster && !writesRenderTarget && !entry.info.hasSideEffects) {
            ReportValidationError("Raster pass '" + entry.name +
                                  "' writes no color or depth attachment");
        }
        if (entry.kind == PassKind::Compute && writesRenderTarget) {
            ReportValidationError("Compute pass '" + entry.name +
                                  "' writes a render target attachment");
        }
    }

    // Edge from a resource's writer to each of its readers.  A pass that both
    // reads and writes a resource (history buffers) gets no self edge.
    successors_.assign(passes_.size(), {});
    for (usize t = 0; t < textures_.size(); ++t) {
        const u32 writer = textureWriters_[t];
        if (writer == kInvalidHandle) {
            continue;
        }
        for (const u32 reader : textureReaders[t]) {
            if (reader != writer) {
                successors_[writer].push_back(reader);
            }
        }
    }
    for (usize b = 0; b < buffers_.size(); ++b) {
        const u32 writer = bufferWriters_[b];
        if (writer == kInvalidHandle) {
            continue;
        }
        for (const u32 reader : bufferReaders[b]) {
            if (reader != writer) {
                successors_[writer].push_back(reader);
            }
        }
    }

    // A pass may read several resources from the same producer; keep one edge.
    incoming_.assign(passes_.size(), 0);
    stats_.edges = 0;
    for (std::vector<u32>& edges : successors_) {
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        for (const u32 next : edges) {
            ++incoming_[next];
        }
        stats_.edges += static_cast<u32>(edges.size());
    }
    return {};
}

void RenderGraph::CullPasses() {
    // A pass survives when something downstream needs its output: an output
    // resource, a side effect, or a kept reader.
    std::vector<bool> keep(passes_.size(), false);
    std::vector<u32> worklist;
    for (u32 i = 0; i < passes_.size(); ++i) {
        keep[i] = passes_[i].info.hasSideEffects;
        if (keep[i]) {
            worklist.push_back(i);
        }
    }
    for (usize t = 0; t < textures_.size(); ++t) {
        if (!textures_[t].info.isOutput) {
            continue;
        }
        const u32 writer = textureWriters_[t];
        if (writer != kInvalidHandle && !keep[writer]) {
            keep[writer] = true;
            worklist.push_back(writer);
        }
    }
    for (usize b = 0; b < buffers_.size(); ++b) {
        if (!buffers_[b].info.isOutput) {
            continue;
        }
        const u32 writer = bufferWriters_[b];
        if (writer != kInvalidHandle && !keep[writer]) {
            keep[writer] = true;
            worklist.push_back(writer);
        }
    }

    // Walk the dependency edges backwards: anything a kept pass reads must run.
    while (!worklist.empty()) {
        const u32 passIndex = worklist.back();
        worklist.pop_back();
        for (const ResourceAccess& access : passes_[passIndex].accesses) {
            u32 writer = kInvalidHandle;
            if (access.isTexture) {
                writer = textureWriters_[access.resource];
            } else {
                writer = bufferWriters_[access.resource];
            }
            if (writer != kInvalidHandle && writer != passIndex && !keep[writer]) {
                keep[writer] = true;
                worklist.push_back(writer);
            }
        }
    }

    stats_.culledPasses = 0;
    for (u32 i = 0; i < passes_.size(); ++i) {
        passes_[i].info.culled = !keep[i];
        if (!keep[i]) {
            ++stats_.culledPasses;
        }
    }
}

Result<void> RenderGraph::TopologicalSort() {
    // Kahn's algorithm over the kept passes.  Ties are broken by declaration
    // order, so hazards the graph does not model explicitly (write after read)
    // keep the order the caller wrote them in.
    std::vector<u32> ready;
    for (u32 i = 0; i < passes_.size(); ++i) {
        if (!passes_[i].info.culled && incoming_[i] == 0) {
            ready.push_back(i);
        }
    }
    std::sort(ready.begin(), ready.end());

    executionOrder_.clear();
    std::vector<u32> remainingIncoming = incoming_;
    while (!ready.empty()) {
        const u32 passIndex = ready.front();
        ready.erase(ready.begin());
        executionOrder_.push_back(PassHandle{passIndex});
        passes_[passIndex].info.orderIndex = static_cast<u32>(executionOrder_.size() - 1);
        for (const u32 next : successors_[passIndex]) {
            if (passes_[next].info.culled) {
                continue;
            }
            if (remainingIncoming[next] > 0) {
                --remainingIncoming[next];
            }
            if (remainingIncoming[next] == 0) {
                ready.push_back(next);
                std::sort(ready.begin(), ready.end());
            }
        }
    }

    if (executionOrder_.size() != passes_.size() - stats_.culledPasses) {
        std::string involved;
        for (u32 i = 0; i < passes_.size(); ++i) {
            if (!passes_[i].info.culled && passes_[i].info.orderIndex == kInvalidHandle) {
                if (!involved.empty()) {
                    involved += ", ";
                }
                involved += passes_[i].name;
            }
        }
        return Unexpected(Status{StatusCode::InvalidState,
                                 "Render graph has a dependency cycle involving: " + involved});
    }
    return {};
}

void RenderGraph::ComputeLifetimes() {
    for (TextureEntry& entry : textures_) {
        entry.info.firstUse = kInvalidHandle;
        entry.info.lastUse = 0;
    }
    for (BufferEntry& entry : buffers_) {
        entry.info.firstUse = kInvalidHandle;
        entry.info.lastUse = 0;
    }

    for (u32 order = 0; order < executionOrder_.size(); ++order) {
        const PassEntry& entry = passes_[executionOrder_[order].Index()];
        for (const ResourceAccess& access : entry.accesses) {
            ResourceInfo& info = access.isTexture ? textures_[access.resource].info
                                                  : buffers_[access.resource].info;
            if (info.firstUse == kInvalidHandle) {
                info.firstUse = order;
            }
            info.lastUse = order;
        }
    }
}

Result<void> RenderGraph::Compile() {
    validationErrors_ = 0;
    validationErrorList_.clear();
    compiled_ = false;
    executionOrder_.clear();
    stats_ = CompileStats{};

    RunSetupCallbacks();
    if (auto result = BuildDependencies(); result.IsError()) {
        return result;
    }
    CullPasses();
    if (auto result = TopologicalSort(); result.IsError()) {
        return result;
    }
    ComputeLifetimes();

    stats_.declaredPasses = static_cast<u32>(passes_.size());
    stats_.executedPasses = static_cast<u32>(executionOrder_.size());
    stats_.textureCount = static_cast<u32>(textures_.size());
    stats_.bufferCount = static_cast<u32>(buffers_.size());
    stats_.transientTextureCount = 0;
    stats_.transientBufferCount = 0;
    stats_.transientBytes = 0;
    for (const TextureEntry& entry : textures_) {
        if (entry.info.isExternal) {
            continue;
        }
        ++stats_.transientTextureCount;
        stats_.transientBytes += entry.info.sizeBytes;
    }
    for (const BufferEntry& entry : buffers_) {
        if (entry.info.isExternal) {
            continue;
        }
        ++stats_.transientBufferCount;
        stats_.transientBytes += entry.info.sizeBytes;
    }

    compiled_ = true;
    return {};
}

// --- Execution -------------------------------------------------------------

Result<rhi::ITexture*> RenderGraph::ResolveTexture(u32 index, rhi::IDevice& device) {
    TextureEntry& entry = textures_[index];
    if (entry.external != nullptr) {
        return entry.external;
    }
    if (entry.owned) {
        return entry.owned.get();
    }

    const CacheKey key{entry.spec.width,       entry.spec.height,
                       entry.spec.mipLevels,   entry.spec.arrayLayers,
                       entry.spec.format,      entry.spec.dimension,
                       entry.spec.usage,       entry.spec.samples};
    for (usize i = 0; i < textureCache_.size(); ++i) {
        if (textureCache_[i].first == key) {
            // Take it out of the cache: while a frame uses it, nobody else may.
            entry.owned = std::move(textureCache_[i].second);
            textureCache_.erase(textureCache_.begin() + static_cast<isize>(i));
            return entry.owned.get();
        }
    }

    auto created = device.CreateTexture(ToRhiDesc(entry.spec));
    if (created.IsError()) {
        ReportValidationError("Failed to create transient texture '" + entry.spec.name +
                              "': " + std::string(created.Error().Message()));
        return Unexpected(created.Error());
    }
    entry.owned = std::move(*created);
    return entry.owned.get();
}

Result<rhi::IBuffer*> RenderGraph::ResolveBuffer(u32 index, rhi::IDevice& device) {
    BufferEntry& entry = buffers_[index];
    if (entry.external != nullptr) {
        return entry.external;
    }
    if (entry.owned) {
        return entry.owned.get();
    }

    const BufferCacheKey key{entry.spec.size, entry.spec.usage};
    for (usize i = 0; i < bufferCache_.size(); ++i) {
        if (bufferCache_[i].first == key) {
            entry.owned = std::move(bufferCache_[i].second);
            bufferCache_.erase(bufferCache_.begin() + static_cast<isize>(i));
            return entry.owned.get();
        }
    }

    auto created = device.CreateBuffer(ToRhiDesc(entry.spec));
    if (created.IsError()) {
        ReportValidationError("Failed to create transient buffer '" + entry.spec.name +
                              "': " + std::string(created.Error().Message()));
        return Unexpected(created.Error());
    }
    entry.owned = std::move(*created);
    return entry.owned.get();
}

Result<void> RenderGraph::Execute(rhi::IDevice& device, rhi::ICommandBuffer& commands) {
    if (!compiled_) {
        return Unexpected(Status{StatusCode::InvalidState,
                                 "RenderGraph::Execute called before a successful Compile()"});
    }

    for (const PassHandle handle : executionOrder_) {
        PassEntry& entry = passes_[handle.Index()];
        PassContext context(device, commands, entry.info, entry.accesses);
        for (const ResourceAccess& access : entry.accesses) {
            if (access.isTexture) {
                auto resolved = ResolveTexture(access.resource, device);
                if (resolved.IsError()) {
                    return Unexpected(resolved.Error());
                }
                context.BindTexture(access.resource, *resolved);
            } else {
                auto resolved = ResolveBuffer(access.resource, device);
                if (resolved.IsError()) {
                    return Unexpected(resolved.Error());
                }
                context.BindBuffer(access.resource, *resolved);
            }
        }

        commands.BeginDebugLabel(entry.name);
        if (entry.execute) {
            entry.execute(context);
        }
        commands.EndDebugLabel();

        if (context.UndeclaredAccesses() > 0) {
            validationErrors_ += context.UndeclaredAccesses();
            validationErrorList_.push_back("Pass '" + entry.name + "' made " +
                                           std::to_string(context.UndeclaredAccesses()) +
                                           " undeclared resource accesses");
        }
    }

    // Hand the transients back to the cache so the next frame reuses them.
    for (TextureEntry& entry : textures_) {
        if (entry.owned) {
            const CacheKey key{entry.spec.width,     entry.spec.height,
                               entry.spec.mipLevels, entry.spec.arrayLayers,
                               entry.spec.format,    entry.spec.dimension,
                               entry.spec.usage,     entry.spec.samples};
            textureCache_.emplace_back(key, std::move(entry.owned));
        }
    }
    for (BufferEntry& entry : buffers_) {
        if (entry.owned) {
            bufferCache_.emplace_back(BufferCacheKey{entry.spec.size, entry.spec.usage},
                                      std::move(entry.owned));
        }
    }
    return {};
}

void RenderGraph::ReleaseTransientResources() {
    textureCache_.clear();
    bufferCache_.clear();
    for (TextureEntry& entry : textures_) {
        entry.owned.reset();
    }
    for (BufferEntry& entry : buffers_) {
        entry.owned.reset();
    }
}

// --- Accessors -------------------------------------------------------------

const PassInfo& RenderGraph::Pass(PassHandle pass) const {
    static const PassInfo invalid{};
    return pass.Index() < passes_.size() ? passes_[pass.Index()].info : invalid;
}

const ResourceInfo& RenderGraph::Texture(TextureHandle handle) const {
    static const ResourceInfo invalid{};
    return IsValidTexture(handle.Index()) ? textures_[handle.Index()].info : invalid;
}

const ResourceInfo& RenderGraph::Buffer(BufferHandle handle) const {
    static const ResourceInfo invalid{};
    return IsValidBuffer(handle.Index()) ? buffers_[handle.Index()].info : invalid;
}

std::span<const ResourceAccess> RenderGraph::PassAccesses(PassHandle pass) const {
    if (pass.Index() >= passes_.size()) {
        return {};
    }
    return passes_[pass.Index()].accesses;
}

void RenderGraph::ReportValidationError(std::string message) {
    ++validationErrors_;
    L3D_LOG_ERROR(LogCategory::RenderGraph, "{}", message);
    validationErrorList_.push_back(std::move(message));
}

} // namespace l3d::graph
