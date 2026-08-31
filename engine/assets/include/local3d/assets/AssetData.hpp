#pragma once
/// @file AssetData.hpp
/// @brief The plain data types the asset pipeline produces and the rest of the
///        engine consumes.
///
/// These live in the asset module rather than in the renderer because they are
/// produced by importers and consumed by several systems: the renderer uploads
/// `MeshData`, the audio mixer streams `AudioData`, the editor previews all of
/// them.  Keeping them here means the dependency direction stays
/// Assets <- Renderer instead of the other way round.
///
/// Everything in this header is value data with no handles, no device pointers
/// and no lifetime rules: it can be moved between threads freely.

#include "local3d/core/Common.hpp"
#include "local3d/math/Geometry.hpp"
#include "local3d/math/Vector.hpp"
#include "local3d/rhi/RhiTypes.hpp"

#include <string>
#include <vector>

namespace l3d::assets {

/// CPU side geometry.  Importers fill this in; the renderer uploads it.
///
/// The three attribute arrays are always the same length - see IsValid() - and
/// the index buffer is a triangle list.  Splitting the arrays (instead of one
/// interleaved struct) keeps importers simple and lets the renderer choose the
/// packing it wants.
struct MeshData {
    std::string name;
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;
    std::vector<math::Vec2> uvs;
    std::vector<u32> indices;
    math::Aabb bounds;
    /// Index into the document's material table, or -1 when the source did not
    /// assign one.  The renderer ignores it; the scene layer uses it to bind a
    /// material.  Dropping it would silently lose a glTF primitive's material.
    i32 materialIndex = -1;

    [[nodiscard]] bool IsValid() const noexcept {
        return !positions.empty() && positions.size() == normals.size() &&
               positions.size() == uvs.size() && indices.size() >= 3 &&
               indices.size() % 3 == 0;
    }
};

/// How an image should be interpreted once imported.
struct TextureImportSettings {
    /// Albedo/emissive textures are sRGB encoded; normal, mask and data
    /// textures are not.  Picking this wrong is the classic "why does my normal
    /// map look broken" bug, so it is an explicit per-asset setting.
    bool srgb = true;
    /// Build the full mip chain with a box filter at import time.  Runtime mip
    /// generation costs frames; import time does not.
    bool generateMips = true;
    /// Downscale the base mip so no dimension exceeds this.  0 keeps the source
    /// resolution.
    u32 maxSize = 0;
    /// Treat alpha as coverage for cutouts.  Currently only recorded, but the
    /// material importer uses it to reject mip generation that would bleed.
    bool alphaIsCoverage = false;

    /// Stable hash so the cooker can skip work when nothing changed.
    [[nodiscard]] u64 Hash() const noexcept;
};

/// Number of mips in a full chain for a base size, including mip 0.
[[nodiscard]] constexpr u32 MipChainLength(u32 width, u32 height) noexcept {
    u32 size = width > height ? width : height;
    u32 levels = 1;
    while (size > 1) {
        size >>= 1;
        ++levels;
    }
    return levels;
}

/// A decoded image with its full mip chain, ready for upload.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    u32 mipLevels = 1;
    rhi::Format format = rhi::Format::RGBA8_UNorm;
    /// All mips concatenated, mip 0 first, tightly packed rows.
    std::vector<u8> pixels;

    [[nodiscard]] u32 MipWidth(u32 mip) const noexcept {
        const u32 size = width >> (mip < mipLevels ? mip : mipLevels - 1);
        return size == 0 ? 1 : size;
    }
    [[nodiscard]] u32 MipHeight(u32 mip) const noexcept {
        const u32 size = height >> (mip < mipLevels ? mip : mipLevels - 1);
        return size == 0 ? 1 : size;
    }
    /// Byte offset of a mip inside `pixels`.
    [[nodiscard]] usize MipOffset(u32 mip) const noexcept;
    /// Byte size of a mip.
    [[nodiscard]] usize MipBytes(u32 mip) const noexcept;
    /// Bytes per pixel of the (uncompressed) format; 0 for block formats.
    [[nodiscard]] u32 BytesPerPixel() const noexcept;

    [[nodiscard]] bool IsValid() const noexcept {
        return width > 0 && height > 0 && mipLevels > 0 && !pixels.empty() &&
               pixels.size() >= MipBytes(0);
    }
};

/// Decoded audio, always 32 bit float interleaved.
///
/// Importers normalise every supported source format to float so the mixer has
/// exactly one sample type to deal with.  Memory is the price; streaming keeps
/// it bounded for music, and short effects are small either way.
struct AudioData {
    u32 sampleRate = 0;
    u32 channels = 0;
    /// Frames = samples per channel.
    u32 frameCount = 0;
    /// Interleaved, frame major: [L0 R0 L1 R1 ...].  Size frameCount*channels.
    std::vector<f32> samples;
    std::string name;

    [[nodiscard]] f64 DurationSeconds() const noexcept {
        return sampleRate == 0 ? 0.0 : static_cast<f64>(frameCount) / static_cast<f64>(sampleRate);
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return sampleRate > 0 && channels > 0 && frameCount > 0 &&
               samples.size() == static_cast<usize>(frameCount) * channels;
    }
};

/// A PBR metallic-roughness material, the model glTF and the renderer agree on.
///
/// Texture references are *asset paths*, resolved to ids by the asset database
/// once the importing document is registered.  Importers never see ids.
struct MaterialData {
    std::string name;
    math::Vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    math::Vec3 emissive{0.0f, 0.0f, 0.0f};
    f32 normalScale = 1.0f;
    f32 alphaCutoff = 0.5f;
    bool doubleSided = false;

    std::string baseColorTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    std::string emissiveTexture;

    [[nodiscard]] bool HasAnyTexture() const noexcept {
        return !baseColorTexture.empty() || !normalTexture.empty() ||
               !metallicRoughnessTexture.empty() || !emissiveTexture.empty();
    }
};

/// Textures that live inside a container file (a GLB's BIN chunk) are named
/// "embedded:<name>" in material references; the cooker turns them into real
/// texture assets.
inline constexpr std::string_view kEmbeddedTexturePrefix = "embedded:";

struct EmbeddedImage {
    std::string name;
    /// "image/png" or "image/jpeg".
    std::string mimeType;
    std::vector<u8> bytes;
};

/// Everything an importer produced from one source file.
///
/// A GLB can contain several meshes and several materials, so the document is a
/// list rather than a single object; the cooker splits it into one cooked asset
/// per sub-asset with deterministic ids derived from the source id and the
/// sub-asset index.
struct MeshDocument {
    std::string name;
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<EmbeddedImage> embeddedImages;

    /// Index of an embedded image by name, or -1.
    [[nodiscard]] i32 FindEmbeddedImage(std::string_view imageName) const noexcept {
        for (usize i = 0; i < embeddedImages.size(); ++i) {
            if (embeddedImages[i].name == imageName) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
};

struct TextureDocument {
    ImageData image;
    TextureImportSettings settings;
};

struct AudioDocument {
    AudioData audio;
};

} // namespace l3d::assets
