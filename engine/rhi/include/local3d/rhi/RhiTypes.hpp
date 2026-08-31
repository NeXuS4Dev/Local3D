#pragma once
/// @file RhiTypes.hpp
/// @brief Backend independent descriptions of GPU state.
///
/// Design rules (docs/architecture/rhi.md):
///  * No backend type ever appears in a public header.  Vulkan/DX12/Metal types
///    live in the backend translation units only.
///  * Descriptors are plain data, cheap to copy, and validated by the backend.
///  * Everything is expressed in terms a modern explicit API can map to 1:1
///    (Vulkan today; D3D12 and Metal later) so no information is lost.

#include "local3d/containers/SmallVector.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Enum.hpp"
#include "local3d/math/Math.hpp"

#include <string>
#include <string_view>

namespace l3d::rhi {

/// Which backend created a device.
enum class BackendType : u8 {
    Null = 0, ///< CPU only reference/validation backend.  Always available.
    Vulkan,
    D3D12, ///< Reserved; not implemented yet.
    Metal, ///< Reserved; not implemented yet.
};

[[nodiscard]] std::string_view BackendTypeName(BackendType type) noexcept;

/// Texture and vertex formats.  Deliberately a small, well supported subset.
enum class Format : u16 {
    Unknown = 0,
    R8_UNorm,
    RG8_UNorm,
    RGBA8_UNorm,
    RGBA8_SRGB,
    BGRA8_UNorm,
    R16_Float,
    RG16_Float,
    RGBA16_Float,
    R32_Float,
    RG32_Float,
    RGBA32_Float,
    R10G10B10A2_UNorm,
    R11G11B10_Float,
    RGB32_Float, ///< Unpacked float3: the vertex attribute workhorse.

    Depth16_UNorm,
    Depth24_Stencil8,
    Depth32_Float,
    BC1_RGBA_UNorm,
    BC3_RGBA_UNorm,
    BC5_RG_UNorm,
    BC7_RGBA_UNorm,
    Count,
};

struct FormatInfo {
    u32 bytesPerBlock = 0;
    u32 blockWidth = 1;
    u32 blockHeight = 1;
    bool hasDepth = false;
    bool hasStencil = false;
    bool isSrgb = false;
    bool isCompressed = false;
    std::string_view name = "Unknown";
};

[[nodiscard]] const FormatInfo& GetFormatInfo(Format format) noexcept;
[[nodiscard]] bool IsDepthFormat(Format format) noexcept;
[[nodiscard]] u32 MipLevelSize(u32 baseSize, u32 mipLevel) noexcept;
/// Byte size of one mip level of a 2D texture (compressed aware).
[[nodiscard]] u64 ComputeMipBytes(Format format, u32 width, u32 height, u32 mipLevel) noexcept;
/// Total bytes for all mip levels.
[[nodiscard]] u64 ComputeTextureBytes(const struct TextureDesc& desc) noexcept;

enum class TextureDimension : u8 { Tex1D, Tex2D, Tex3D, Cube };
enum class SampleCount : u8 { One = 1, Two = 2, Four = 4, Eight = 8 };

enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Indirect = 1u << 4,
    CopySource = 1u << 5,
    CopyDestination = 1u << 6,
};
L3D_FLAGS_ENUM(BufferUsage)

enum class TextureUsage : u32 {
    None = 0,
    Sampled = 1u << 0,
    ColorAttachment = 1u << 1,
    DepthStencilAttachment = 1u << 2,
    Storage = 1u << 3,
    CopySource = 1u << 4,
    CopyDestination = 1u << 5,
};
L3D_FLAGS_ENUM(TextureUsage)

enum class MemoryType : u8 {
    GpuOnly = 0,       ///< Fastest; upload through a staging buffer.
    CpuToGpu,          ///< Write combined / upload heap.
    GpuToCpu,          ///< Read back results (query pools, screenshots).
};

enum class ShaderStage : u8 { Vertex, Fragment, Compute };
/// Descriptor bindings and pipeline layouts talk about *sets* of stages.
L3D_FLAGS_ENUM(ShaderStage)

/// Shader bytecode format expected by a backend.
enum class ShaderFormat : u8 {
    Spirv = 0,
    Dxil,   ///< Reserved for the D3D12 backend.
    Metallib, ///< Reserved for the Metal backend.
};

enum class PrimitiveTopology : u8 { TriangleList, TriangleStrip, LineList, LineStrip, PointList };

enum class CullMode : u8 { None = 0, Front = 1, Back = 2, FrontAndBack = 3 };
enum class FrontFace : u8 { CounterClockwise = 0, Clockwise = 1 };
enum class CompareOp : u8 { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class BlendFactor : u8 {
    Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, ConstantColor,
};
enum class BlendOp : u8 { Add, Subtract, ReverseSubtract, Min, Max };
enum class LoadOp : u8 { Load, Clear, DontCare };
enum class StoreOp : u8 { Store, DontCare };
enum class Filter : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
enum class DescriptorType : u8 {
    UniformBuffer = 0,
    StorageBuffer,
    SampledImage,
    CombinedImageSampler,
    StorageImage,
    Sampler,
};

struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    Format format = Format::RGBA32_Float;
    u32 offset = 0;
};

struct VertexBinding {
    u32 binding = 0;
    u32 stride = 0;
    bool perInstance = false;
};

struct PushConstantRange {
    ShaderStage stage = ShaderStage::Vertex;
    u32 offset = 0;
    u32 size = 0;
};

struct RasterizerState {
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    bool depthClamp = false;
    bool wireframe = false;
    f32 lineWidth = 1.0f;
};

struct DepthStencilState {
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompare = CompareOp::Less;
    bool stencilTest = false;
};

struct BlendState {
    bool enabled = false;
    BlendFactor srcColor = BlendFactor::SrcAlpha;
    BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::OneMinusSrcAlpha;
    BlendOp alphaOp = BlendOp::Add;
};

struct Viewport {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 1.0f;
    f32 height = 1.0f;
    f32 minDepth = 0.0f;
    f32 maxDepth = 1.0f;
};

struct Rect2D {
    i32 x = 0;
    i32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

struct ClearValue {
    math::Color color{0.0f, 0.0f, 0.0f, 1.0f};
    f32 depth = 1.0f;
    u32 stencil = 0;
    bool clearDepth = true;
    bool clearColor = true;
};

// --- Resource descriptors --------------------------------------------------

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryType memory = MemoryType::GpuOnly;
    std::string debugName;
};

struct TextureDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    Format format = Format::RGBA8_UNorm;
    TextureDimension dimension = TextureDimension::Tex2D;
    TextureUsage usage = TextureUsage::Sampled;
    SampleCount samples = SampleCount::One;
    std::string debugName;
};

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    f32 maxAnisotropy = 1.0f;
    bool compareEnable = false;
    CompareOp compareOp = CompareOp::Less;
    f32 minLod = 0.0f;
    f32 maxLod = 16.0f;
    std::string debugName;
};

struct ShaderModuleDesc {
    ShaderStage stage = ShaderStage::Vertex;
    ShaderFormat format = ShaderFormat::Spirv;
    ConstByteSpan bytecode;
    std::string entryPoint = "main";
    std::string debugName;
};

struct DescriptorBinding {
    u32 binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    ShaderStage stages = ShaderStage::Vertex;
    u32 count = 1;
};

struct DescriptorSetLayoutDesc {
    SmallVector<DescriptorBinding, 8> bindings;
    std::string debugName;
};

struct AttachmentDesc {
    Format format = Format::RGBA16_Float;
    SampleCount samples = SampleCount::One;
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
};

struct RenderPassDesc {
    SmallVector<AttachmentDesc, 4> colorAttachments;
    AttachmentDesc depthStencil;
    bool hasDepthStencil = false;
    std::string debugName;
};

struct PipelineDesc {
    SmallVector<ShaderModuleDesc, 2> shaders;
    SmallVector<VertexBinding, 4> vertexBindings;
    SmallVector<VertexAttribute, 8> vertexAttributes;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterizerState rasterizer;
    DepthStencilState depthStencil;
    BlendState blend;
    /// Attachment formats the pipeline is compatible with.
    SmallVector<Format, 4> colorFormats;
    Format depthFormat = Format::Unknown;
    bool hasDepthAttachment = false;
    SmallVector<PushConstantRange, 2> pushConstantRanges;
    SmallVector<u32, 4> descriptorSetLayoutIds; ///< Layout handles from the device.
    std::string debugName;
};

struct SwapchainDesc {
    void* windowHandle = nullptr;
    u32 width = 1280;
    u32 height = 720;
    Format format = Format::BGRA8_UNorm;
    u32 imageCount = 3;
    bool vsync = true;
};

/// Statistics about a device, mostly for the editor's renderer panel.
struct DeviceInfo {
    std::string deviceName = "Local3D Null Device";
    BackendType backend = BackendType::Null;
    std::string apiVersion = "0.0";
    u32 maxTextureSize2D = 16384;
    u32 maxMipLevels = 15;
    u32 maxColorAttachments = 8;
    u32 maxUniformBufferRange = 65536;
    u32 maxPushConstantSize = 128;
    u32 maxBoundDescriptorSets = 4;
    u64 maxBufferSize = 1ULL << 30;
    bool supportsTimestampQueries = true;
    bool supportsIndirectDraw = true;
    bool supportsCompute = true;
    bool supportsAnisotropicFiltering = true;
    bool supportsBcTextures = true;
    bool validationEnabled = false;
};

} // namespace l3d::rhi
