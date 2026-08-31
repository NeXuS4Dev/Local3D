#pragma once
/// @file RenderGraphTypes.hpp
/// @brief Handles, descriptors and statistics shared by the render graph.
///
/// The graph speaks in *handles*, never in pointers: a resource may not exist
/// yet when a pass is declared, and its backing memory changes between frames.
/// Handles are strongly typed (a TextureHandle is not a BufferHandle) and are
/// only valid inside the graph that produced them.

#include "local3d/core/Common.hpp"
#include "local3d/rhi/RhiTypes.hpp"

#include <string>

namespace l3d::graph {

/// Tag types so that Handle<TextureTag> and Handle<BufferTag> cannot be mixed.
struct TextureTag {};
struct BufferTag {};
struct PassTag {};

/// Invalid handle sentinel.
inline constexpr u32 kInvalidHandle = 0xFFFF'FFFF;

/// A typed index into a graph table.  Copyable, comparable, no ownership.
template <typename Tag>
struct Handle {
    u32 index = kInvalidHandle;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return index != kInvalidHandle; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }
    [[nodiscard]] constexpr u32 Index() const noexcept { return index; }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;
};

using TextureHandle = Handle<TextureTag>;
using BufferHandle = Handle<BufferTag>;
using PassHandle = Handle<PassTag>;

/// A graph level texture description.  It maps 1:1 onto rhi::TextureDesc; the
/// extra information is what the graph needs to reason about the resource.
struct TextureSpec {
    std::string name = "texture";
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    rhi::Format format = rhi::Format::RGBA16_Float;
    /// Array-ness lives in arrayLayers; dimension distinguishes 2D/3D/cube.
    rhi::TextureDimension dimension = rhi::TextureDimension::Tex2D;
    rhi::TextureUsage usage = rhi::TextureUsage::Sampled;
    rhi::SampleCount samples = rhi::SampleCount::One;
};

/// A graph level buffer description.
struct BufferSpec {
    std::string name = "buffer";
    u64 size = 0;
    rhi::BufferUsage usage = rhi::BufferUsage::Storage;
};

[[nodiscard]] rhi::TextureDesc ToRhiDesc(const TextureSpec& spec);
[[nodiscard]] rhi::BufferDesc ToRhiDesc(const BufferSpec& spec);

/// What a pass does.  The kind decides which command buffer calls are legal and
/// how the graph treats its accesses.
enum class PassKind : u8 {
    Raster, ///< Records inside a render pass (draws).
    Compute, ///< Records dispatches.
    Copy, ///< Records transfers only.
};

[[nodiscard]] std::string_view PassKindName(PassKind kind) noexcept;

/// How a pass touches a resource.  Recorded per access so the graph can build
/// the dependency edges and so Execute can validate command recording.
enum class AccessKind : u8 {
    Read,
    Write,
    ReadWrite,
};

/// One declared resource access.
struct ResourceAccess {
    u32 resource = kInvalidHandle; ///< Index into the texture *or* buffer table.
    bool isTexture = true;
    AccessKind kind = AccessKind::Read;
};

/// Per-resource bookkeeping produced by Compile().
struct ResourceInfo {
    std::string name;
    bool isTexture = true;
    bool isExternal = false; ///< Imported by the application, not allocated here.
    bool isOutput = false; ///< Kept alive by the graph even if nothing reads it.
    u64 sizeBytes = 0;
    /// Pass indices (into the execution order) that first and last touch this
    /// resource.  `lastUse < firstUse` means the resource was never used.
    u32 firstUse = kInvalidHandle;
    u32 lastUse = 0;
    u32 writerCount = 0;
    u32 readerCount = 0;
};

/// Per-pass bookkeeping produced by Compile().
struct PassInfo {
    std::string name;
    PassKind kind = PassKind::Raster;
    bool culled = false; ///< Removed because nothing consumes its output.
    bool hasSideEffects = false; ///< Never culled (present, query readback, ...).
    u32 orderIndex = kInvalidHandle; ///< Position in the execution order.
};

/// What a compile produced.  Used by tests, the frame debugger and the editor's
/// render graph view.
struct CompileStats {
    u32 declaredPasses = 0;
    u32 executedPasses = 0;
    u32 culledPasses = 0;
    u32 textureCount = 0;
    u32 bufferCount = 0;
    u32 transientTextureCount = 0;
    u32 transientBufferCount = 0;
    u64 transientBytes = 0;
    u32 edges = 0;
};

} // namespace l3d::graph
