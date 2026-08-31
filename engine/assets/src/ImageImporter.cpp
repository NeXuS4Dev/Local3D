/// @file ImageImporter.cpp
/// @brief Imports raster images (PNG, JPEG, TGA, BMP, Radiance HDR) into ImageData.
///
/// Decoding is delegated to stb_image (see docs/architecture/dependencies.md);
/// everything around it is ours: colour space handling, mip generation and the
/// choice of GPU format.
///
/// Colour space matters here and is easy to get wrong.  An sRGB texture stores
/// *encoded* values, so filtering them directly blends in a non linear space and
/// darkens gradients.  This importer therefore:
///   * keeps mip 0 exactly as the source encoded it (no lossy round trip), and
///   * decodes to linear, box filters, and re-encodes for every generated mip
///     when the asset is marked sRGB.
/// Data textures (normal maps, masks) skip the transfer functions entirely.
///
/// HDR sources are stored as RGBA16_Float; 8 bit sources as RGBA8 (sRGB or
/// UNorm).  Everything is normalised to four channels so the renderer never has
/// to care how many the source had.

#include "local3d/assets/Importer.hpp"

#include "local3d/assets/AssetMeta.hpp"

#include "local3d/core/Log.hpp"

#include <cmath>
#include <cstring>
#include <memory>

#include "stb_image.h"

namespace l3d::assets {

namespace {

[[nodiscard]] f32 SrgbToLinear(f32 encoded) noexcept {
    if (encoded <= 0.04045f) {
        return encoded / 12.92f;
    }
    const f32 shifted = (encoded + 0.055f) / 1.055f;
    // pow is the definition; 2.4 is the exponent the standard specifies.
    return static_cast<f32>(std::pow(static_cast<f64>(shifted), 2.4));
}

[[nodiscard]] f32 LinearToSrgb(f32 linear) noexcept {
    if (linear <= 0.0031308f) {
        return linear * 12.92f;
    }
    const f64 root = std::pow(static_cast<f64>(linear), 1.0 / 2.4);
    return static_cast<f32>(1.055 * root - 0.055);
}

/// Round to nearest, ties to even, then flush values outside the half range.
/// Image data never needs subnormals, so flushing them costs nothing.
[[nodiscard]] u16 FloatToHalf(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 31) & 0x1U;
    const i32 exponent = static_cast<i32>((bits >> 23) & 0xFFU) - 127;
    const u32 mantissa = bits & 0x7FFFFFU;

    if (exponent == 128) { // Infinity or NaN.
        return static_cast<u16>((sign << 15) | 0x7C00U | (mantissa != 0 ? 0x200U : 0U));
    }
    if (exponent > 15) { // Overflow saturates to infinity.
        return static_cast<u16>((sign << 15) | 0x7C00U);
    }
    if (exponent < -14) { // Underflow flushes to signed zero.
        return static_cast<u16>(sign << 15);
    }

    const u32 dropped = mantissa & 0x1FFFU;
    u32 half = (sign << 15) | (static_cast<u32>(exponent + 15) << 10) | (mantissa >> 13);
    if (dropped > 0x1000U || (dropped == 0x1000U && (half & 0x1U) != 0)) {
        ++half;
    }
    return static_cast<u16>(half);
}

[[nodiscard]] u8 Quantize8(f32 value) noexcept {
    const f32 clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<u8>(clamped * 255.0f + 0.5f);
}

/// An RGBA float image.  All filtering happens in this representation.
struct FloatImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> pixels; ///< 4 per texel.

    [[nodiscard]] f32* Texel(u32 x, u32 y) noexcept {
        return pixels.data() + (static_cast<usize>(y) * width + x) * 4;
    }
    [[nodiscard]] const f32* Texel(u32 x, u32 y) const noexcept {
        return pixels.data() + (static_cast<usize>(y) * width + x) * 4;
    }
};

/// 2x2 box filter with edge replication, so odd sizes do not lose the last row.
[[nodiscard]] FloatImage Downsample(const FloatImage& src) {
    FloatImage dst;
    dst.width = src.width > 1 ? src.width / 2 : 1;
    dst.height = src.height > 1 ? src.height / 2 : 1;
    dst.pixels.resize(static_cast<usize>(dst.width) * dst.height * 4, 0.0f);

    for (u32 y = 0; y < dst.height; ++y) {
        for (u32 x = 0; x < dst.width; ++x) {
            const u32 sx = x * 2;
            const u32 sy = y * 2;
            const u32 sx1 = sx + 1 < src.width ? sx + 1 : sx;
            const u32 sy1 = sy + 1 < src.height ? sy + 1 : sy;
            const f32* a = src.Texel(sx, sy);
            const f32* b = src.Texel(sx1, sy);
            const f32* c = src.Texel(sx, sy1);
            const f32* d = src.Texel(sx1, sy1);
            f32* out = dst.Texel(x, y);
            for (u32 channel = 0; channel < 4; ++channel) {
                out[channel] = (a[channel] + b[channel] + c[channel] + d[channel]) * 0.25f;
            }
        }
    }
    return dst;
}

/// Writes a float image into the packed byte layout of `format`.
[[nodiscard]] std::vector<u8> Quantize(const FloatImage& image, rhi::Format format, bool srgb) {
    const usize texels = static_cast<usize>(image.width) * image.height;
    if (format == rhi::Format::RGBA16_Float) {
        std::vector<u8> bytes(texels * 8);
        for (usize i = 0; i < texels * 4; ++i) {
            const u16 half = FloatToHalf(image.pixels[i]);
            std::memcpy(bytes.data() + i * 2, &half, sizeof(half));
        }
        return bytes;
    }

    std::vector<u8> bytes(texels * 4);
    for (usize i = 0; i < texels * 4; ++i) {
        // Alpha is always linear, even in an sRGB texture: the transfer
        // function is defined for colour, not for coverage.
        const bool isAlpha = (i % 4) == 3;
        const f32 value = (srgb && !isAlpha) ? LinearToSrgb(image.pixels[i]) : image.pixels[i];
        bytes[i] = Quantize8(value);
    }
    return bytes;
}

/// RAII wrapper around stb's C allocation.
struct StbImageDeleter {
    void operator()(void* pointer) const noexcept { stbi_image_free(pointer); }
};
using StbImagePtr = std::unique_ptr<void, StbImageDeleter>;

class ImageImporter final : public IImporter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "image"; }
    [[nodiscard]] u32 Version() const noexcept override { return 1; }
    [[nodiscard]] AssetType OutputType() const noexcept override { return AssetType::Texture; }

    [[nodiscard]] bool CanImport(const AssetPath& path) const noexcept override {
        const std::string extension = path.Extension();
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
               extension == ".tga" || extension == ".bmp" || extension == ".hdr";
    }

    [[nodiscard]] Result<ImportedAsset> Import(ConstByteSpan sourceBytes,
                                              const serial::JsonValue& settings,
                                              const AssetPath& sourcePath,
                                              ImportLog& log) override {
        const TextureImportSettings importSettings = TextureSettingsFromJson(settings);
        const bool isHdr = sourcePath.Extension() == ".hdr";

        int width = 0;
        int height = 0;
        int channels = 0;
        StbImagePtr decoded;
        if (isHdr) {
            decoded = StbImagePtr{stbi_loadf_from_memory(
                reinterpret_cast<const u8*>(sourceBytes.data()),
                static_cast<int>(sourceBytes.size()), &width, &height, &channels, 4)};
        } else {
            decoded = StbImagePtr{stbi_load_from_memory(
                reinterpret_cast<const u8*>(sourceBytes.data()),
                static_cast<int>(sourceBytes.size()), &width, &height, &channels, 4)};
        }
        if (!decoded) {
            const char* reason = stbi_failure_reason();
            return Unexpected(Status{StatusCode::ParseError,
                                     reason != nullptr ? reason : "Image could not be decoded"});
        }
        if (width <= 0 || height <= 0) {
            return Unexpected(Status{StatusCode::ParseError, "Image has zero dimensions"});
        }

        // Mip 0 keeps the source's encoding exactly, so convert through the
        // same scale we will use on the way back out.
        FloatImage base;
        base.width = static_cast<u32>(width);
        base.height = static_cast<u32>(height);
        const usize texelCount = static_cast<usize>(base.width) * base.height;
        base.pixels.resize(texelCount * 4);
        if (isHdr) {
            std::memcpy(base.pixels.data(), decoded.get(), texelCount * 4 * sizeof(f32));
        } else {
            const auto* source = static_cast<const u8*>(decoded.get());
            for (usize i = 0; i < texelCount * 4; ++i) {
                const f32 encoded = static_cast<f32>(source[i]) / 255.0f;
                const bool isAlpha = (i % 4) == 3;
                base.pixels[i] = (importSettings.srgb && !isAlpha) ? SrgbToLinear(encoded)
                                                                  : encoded;
            }
        }

        // Honour the maximum size by halving until we fit.
        while (importSettings.maxSize > 0 &&
               (base.width > importSettings.maxSize || base.height > importSettings.maxSize)) {
            base = Downsample(base);
        }

        const rhi::Format format = isHdr ? rhi::Format::RGBA16_Float
                                         : (importSettings.srgb ? rhi::Format::RGBA8_SRGB
                                                                : rhi::Format::RGBA8_UNorm);

        ImageData image;
        image.width = base.width;
        image.height = base.height;
        image.format = format;
        image.mipLevels =
            importSettings.generateMips ? MipChainLength(base.width, base.height) : 1;
        image.pixels = Quantize(base, format, importSettings.srgb);

        if (importSettings.generateMips) {
            FloatImage current = base;
            for (u32 mip = 1; mip < image.mipLevels; ++mip) {
                current = Downsample(current);
                std::vector<u8> quantized = Quantize(current, format, importSettings.srgb);
                image.pixels.insert(image.pixels.end(), quantized.begin(), quantized.end());
            }
        }

        if (image.pixels.size() != TotalBytes(image)) {
            return Unexpected(
                Status{StatusCode::Internal, "Generated mip chain size does not match the layout"});
        }
        if (!image.IsValid()) {
            return Unexpected(Status{StatusCode::Internal, "Imported image failed validation"});
        }
        if (channels < 4) {
            log.Warning("Image had " + std::to_string(channels) +
                        " channels; alpha was filled with 1.0");
        }

        TextureDocument document;
        document.image = std::move(image);
        document.settings = importSettings;
        return ImportedAsset{std::move(document)};
    }

private:
    [[nodiscard]] usize TotalBytes(const ImageData& image) const noexcept {
        usize total = 0;
        for (u32 mip = 0; mip < image.mipLevels; ++mip) {
            total += image.MipBytes(mip);
        }
        return total;
    }
};

} // namespace

std::unique_ptr<IImporter> CreateImageImporter() {
    return std::make_unique<ImageImporter>();
}

} // namespace l3d::assets
