#include "local3d/rhi/RhiTypes.hpp"

#include "local3d/core/Assert.hpp"

namespace l3d::rhi {

std::string_view BackendTypeName(BackendType type) noexcept {
    switch (type) {
        case BackendType::Null: return "null";
        case BackendType::Vulkan: return "vulkan";
        case BackendType::D3D12: return "d3d12";
        case BackendType::Metal: return "metal";
    }
    return "unknown";
}

const FormatInfo& GetFormatInfo(Format format) noexcept {
    // Table driven: adding a format is one line here and one enum value.
    static constexpr FormatInfo table[] = {
        {0, 1, 1, false, false, false, false, "Unknown"},
        {1, 1, 1, false, false, false, false, "R8_UNorm"},
        {2, 1, 1, false, false, false, false, "RG8_UNorm"},
        {4, 1, 1, false, false, false, false, "RGBA8_UNorm"},
        {4, 1, 1, false, false, true, false, "RGBA8_SRGB"},
        {4, 1, 1, false, false, false, false, "BGRA8_UNorm"},
        {2, 1, 1, false, false, false, false, "R16_Float"},
        {4, 1, 1, false, false, false, false, "RG16_Float"},
        {8, 1, 1, false, false, false, false, "RGBA16_Float"},
        {4, 1, 1, false, false, false, false, "R32_Float"},
        {8, 1, 1, false, false, false, false, "RG32_Float"},
        {16, 1, 1, false, false, false, false, "RGBA32_Float"},
        {4, 1, 1, false, false, false, false, "R10G10B10A2_UNorm"},
        {4, 1, 1, false, false, false, false, "R11G11B10_Float"},
        {12, 1, 1, false, false, false, false, "RGB32_Float"},
        {2, 1, 1, true, false, false, false, "Depth16_UNorm"},
        {4, 1, 1, true, true, false, false, "Depth24_Stencil8"},
        {4, 1, 1, true, false, false, false, "Depth32_Float"},
        {8, 4, 4, false, false, false, true, "BC1_RGBA_UNorm"},
        {16, 4, 4, false, false, false, true, "BC3_RGBA_UNorm"},
        {16, 4, 4, false, false, false, true, "BC5_RG_UNorm"},
        {16, 4, 4, false, false, false, true, "BC7_RGBA_UNorm"},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<usize>(Format::Count),
                  "Format table must cover every Format value in order");
    const usize index = static_cast<usize>(format);
    if (index >= static_cast<usize>(Format::Count)) {
        return table[0];
    }
    return table[index];
}

bool IsDepthFormat(Format format) noexcept { return GetFormatInfo(format).hasDepth; }

u32 MipLevelSize(u32 baseSize, u32 mipLevel) noexcept {
    const u32 size = baseSize >> mipLevel;
    return size > 0 ? size : 1;
}

u64 ComputeMipBytes(Format format, u32 width, u32 height, u32 mipLevel) noexcept {
    const FormatInfo& info = GetFormatInfo(format);
    const u32 mipWidth = MipLevelSize(width, mipLevel);
    const u32 mipHeight = MipLevelSize(height, mipLevel);
    const u32 blocksX = (mipWidth + info.blockWidth - 1) / info.blockWidth;
    const u32 blocksY = (mipHeight + info.blockHeight - 1) / info.blockHeight;
    return static_cast<u64>(blocksX) * blocksY * info.bytesPerBlock;
}

u64 ComputeTextureBytes(const TextureDesc& desc) noexcept {
    u64 bytes = 0;
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        const u64 mipBytes = ComputeMipBytes(desc.format, desc.width, desc.height, mip);
        bytes += mipBytes * desc.depth * desc.arrayLayers;
    }
    return bytes;
}

} // namespace l3d::rhi
