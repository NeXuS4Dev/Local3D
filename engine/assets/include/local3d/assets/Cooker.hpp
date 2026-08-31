#pragma once
/// @file Cooker.hpp
/// @brief Turns imported data into the engine's cooked formats.
///
/// Cooking is the step that makes a shipped game fast to start and small to
/// load: source files (GLB, PNG, WAV) are parsed once, at build time, into flat
/// little endian blobs that the runtime mmaps and uploads without decoding.
///
/// Every cooked file starts with the same header - tag, format version, the
/// source hash and the import fingerprint it was produced from - so a runtime
/// can reject a stale or foreign file instead of reading garbage, and so the
/// cooker can skip work it has already done.
///
/// Cooked files are named by asset id, not by source path, so renaming a source
/// file does not invalidate anything that refers to it.

#include "local3d/assets/AssetDatabase.hpp"
#include "local3d/assets/AssetData.hpp"
#include "local3d/assets/AssetId.hpp"
#include "local3d/assets/FileSystem.hpp"
#include "local3d/assets/Importer.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/serialization/BinaryStream.hpp"
#include "local3d/serialization/Json.hpp"

#include <string>
#include <vector>

namespace l3d::assets {

inline constexpr u32 kCookedFormatVersion = 1;

[[nodiscard]] constexpr u32 CookedTag(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) |
           (static_cast<u32>(static_cast<u8>(d)) << 24);
}

inline constexpr u32 kCookedMeshTag = CookedTag('L', '3', 'D', 'M');
inline constexpr u32 kCookedTextureTag = CookedTag('L', '3', 'D', 'T');
inline constexpr u32 kCookedAudioTag = CookedTag('L', '3', 'D', 'A');
inline constexpr u32 kCookedMaterialTag = CookedTag('L', '3', 'D', 'R');

/// File extension per cooked type.
[[nodiscard]] std::string_view CookedExtension(AssetType type) noexcept;
/// Cooked file name for an asset: "<uuid>.l3dmesh" and friends.
[[nodiscard]] std::string CookedFileName(AssetId id, AssetType type);

// --- Encoding and decoding -------------------------------------------------
//
// The cook and the runtime reader are deliberately next to each other: a field
// added to one and forgotten in the other is the classic silent corruption bug,
// and having them in one file makes that hard to do.

[[nodiscard]] Result<std::vector<u8>> CookMeshDocument(const MeshDocument& document,
                                                       u64 sourceHash, u64 importHash);
[[nodiscard]] Result<MeshDocument> ReadMeshDocument(ConstByteSpan cooked);

[[nodiscard]] Result<std::vector<u8>> CookTextureDocument(const TextureDocument& document,
                                                          u64 sourceHash, u64 importHash);
[[nodiscard]] Result<TextureDocument> ReadTextureDocument(ConstByteSpan cooked);

[[nodiscard]] Result<std::vector<u8>> CookAudioDocument(const AudioDocument& document,
                                                        u64 sourceHash, u64 importHash);
[[nodiscard]] Result<AudioDocument> ReadAudioDocument(ConstByteSpan cooked);

/// A material with its texture references already resolved to ids.
struct CookedMaterial {
    MaterialData data;
    AssetId baseColorTexture;
    AssetId normalTexture;
    AssetId metallicRoughnessTexture;
    AssetId emissiveTexture;

    /// Material slots, in the order they are stored.
    static constexpr u32 kTextureSlots = 4;
    [[nodiscard]] AssetId TextureInSlot(u32 slot) const noexcept;
};

[[nodiscard]] Result<std::vector<u8>> CookMaterial(const CookedMaterial& material, u64 sourceHash,
                                                   u64 importHash);
[[nodiscard]] Result<CookedMaterial> ReadMaterial(ConstByteSpan cooked);

/// Validates the shared header.  Returns false with a status when the tag or
/// version does not match.
[[nodiscard]] Result<void> ReadCookedHeader(serial::BinaryReader& reader, u32 expectedTag);
void WriteCookedHeader(serial::BinaryWriter& writer, u32 tag, u64 sourceHash,
                                     u64 importHash);

// --- Manifest --------------------------------------------------------------

/// One line of the cooked manifest: how the runtime finds an asset.
struct CookedManifestEntry {
    AssetId id;
    /// Source path, kept for the editor ("reveal in asset browser").
    AssetPath sourcePath;
    /// Path inside the cook directory.
    AssetPath cookedPath;
    AssetType type = AssetType::Unknown;
    std::string name;
    /// Ids this asset refers to: a material's textures, today.
    std::vector<AssetId> dependencies;
};

struct CookedManifest {
    u32 formatVersion = kCookedFormatVersion;
    /// Sorted by id so the file diffs cleanly.
    std::vector<CookedManifestEntry> entries;

    [[nodiscard]] const CookedManifestEntry* Find(AssetId id) const noexcept;
    [[nodiscard]] std::string Dump() const;
    [[nodiscard]] static Result<CookedManifest> Parse(std::string_view jsonText);
    void Sort();
};

/// Name of the manifest inside the cook directory.
inline constexpr std::string_view kManifestFileName = "manifest.json";

// --- Cooker ----------------------------------------------------------------

/// One file the cooker produced.
struct CookedAsset {
    AssetId id;
    AssetPath path;
    AssetType type = AssetType::Unknown;
    std::string name;
    std::vector<u8> bytes;
    std::vector<AssetId> dependencies;
};

struct CookReport {
    u32 sourceFilesCooked = 0;
    u32 filesWritten = 0;
    u32 skipped = 0; ///< Already up to date; nothing was rewritten.
    u32 failed = 0;
    std::vector<std::string> cookedPaths;
    std::vector<std::string> skippedPaths;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class Cooker {
public:
    /// @param sourceFs  File system rooted at the asset folder.
    /// @param cookedFs  File system rooted at the cooked output folder.
    [[nodiscard]] Result<void> Initialize(IFileSystem& sourceFs, IFileSystem& cookedFs,
                                          AssetDatabase& database,
                                          const ImporterRegistry& importers);

    /// Cooks everything the database says is out of date and rewrites the
    /// manifest.  Assets that are already current are carried over untouched.
    [[nodiscard]] Result<CookReport> CookAll();

    /// Cooks a single source file into one or more cooked assets.  Exposed so
    /// the editor can reimport one asset after an edit without a full pass.
    [[nodiscard]] Result<std::vector<CookedAsset>> CookRecord(const AssetRecord& record,
                                                             std::vector<std::string>& warnings);

    /// The manifest as it stands after the last CookAll.
    [[nodiscard]] const CookedManifest& Manifest() const noexcept { return manifest_; }

private:
    [[nodiscard]] Result<std::vector<CookedAsset>> CookMeshSource(const AssetRecord& record,
                                                                 ConstByteSpan bytes,
                                                                 std::vector<std::string>& warnings);
    [[nodiscard]] Result<std::vector<CookedAsset>> CookTextureSource(const AssetRecord& record,
                                                                    ConstByteSpan bytes,
                                                                    std::vector<std::string>& warnings);
    [[nodiscard]] Result<std::vector<CookedAsset>> CookAudioSource(const AssetRecord& record,
                                                                  ConstByteSpan bytes,
                                                                  std::vector<std::string>& warnings);

    /// Resolves a material texture reference (a path, or "embedded:<name>") to
    /// the id of the texture asset it will become.
    [[nodiscard]] AssetId ResolveTextureReference(const AssetRecord& record,
                                                  const MeshDocument& document,
                                                  std::string_view reference, u32 subIndexBase,
                                                  std::vector<std::string>& warnings) const;

    IFileSystem* sourceFs_ = nullptr;
    IFileSystem* cookedFs_ = nullptr;
    AssetDatabase* database_ = nullptr;
    const ImporterRegistry* importers_ = nullptr;
    CookedManifest manifest_;
    bool initialized_ = false;
};

} // namespace l3d::assets
