#include "local3d/assets/AssetData.hpp"

#include "local3d/core/Hash.hpp"

namespace l3d::assets {

u64 TextureImportSettings::Hash() const noexcept {
    return HashOf(static_cast<u64>(srgb ? 1 : 0), static_cast<u64>(generateMips ? 1 : 0),
                  static_cast<u64>(maxSize), static_cast<u64>(alphaIsCoverage ? 1 : 0));
}

u32 ImageData::BytesPerPixel() const noexcept {
    const rhi::FormatInfo& info = rhi::GetFormatInfo(format);
    if (info.isCompressed || info.blockWidth != 1 || info.blockHeight != 1) {
        return 0;
    }
    return info.bytesPerBlock;
}

usize ImageData::MipBytes(u32 mip) const noexcept {
    const u32 bpp = BytesPerPixel();
    if (bpp == 0) {
        return 0;
    }
    return static_cast<usize>(MipWidth(mip)) * static_cast<usize>(MipHeight(mip)) * bpp;
}

usize ImageData::MipOffset(u32 mip) const noexcept {
    usize offset = 0;
    for (u32 level = 0; level < mip && level < mipLevels; ++level) {
        offset += MipBytes(level);
    }
    return offset;
}

} // namespace l3d::assets
