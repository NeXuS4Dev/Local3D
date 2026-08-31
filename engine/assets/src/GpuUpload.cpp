#include "local3d/assets/GpuUpload.hpp"

namespace l3d::assets {

Result<rhi::TexturePtr> UploadTexture(rhi::IDevice& device, const ImageData& image,
                                      std::string_view debugName) {
    if (!image.IsValid()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Image data is incomplete"});
    }
    if (image.BytesPerPixel() == 0) {
        return Unexpected(
            Status{StatusCode::Unsupported, "Compressed images cannot be uploaded yet"});
    }

    rhi::TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.mipLevels = image.mipLevels;
    desc.format = image.format;
    desc.dimension = rhi::TextureDimension::Tex2D;
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    desc.debugName = debugName.empty() ? std::string("texture") : std::string(debugName);

    auto texture = device.CreateTexture(desc);
    if (texture.IsError()) {
        return Unexpected(texture.Error());
    }

    // One span per mip, in the layout the cooked file already stores them in.
    std::vector<ConstByteSpan> mips;
    mips.reserve(image.mipLevels);
    for (u32 mip = 0; mip < image.mipLevels; ++mip) {
        const usize offset = image.MipOffset(mip);
        const usize size = image.MipBytes(mip);
        if (offset + size > image.pixels.size()) {
            return Unexpected(
                Status{StatusCode::InvalidArgument, "Image mip chain does not match its layout"});
        }
        mips.emplace_back(reinterpret_cast<const std::byte*>(image.pixels.data()) + offset, size);
    }
    device.UpdateTexture(**texture, mips);
    return texture;
}

} // namespace l3d::assets
