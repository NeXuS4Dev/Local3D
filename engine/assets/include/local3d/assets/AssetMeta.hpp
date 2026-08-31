#pragma once
/// @file AssetMeta.hpp
/// @brief The `.l3dmeta` sidecar: what makes an asset id stable.
///
/// Every imported source file gets a sibling `<file>.l3dmeta` holding its id,
/// its type, the importer that handled it and the settings that were used.  The
/// sidecar is what allows a file to be renamed, moved between folders or copied
/// to another machine without losing its identity - the id travels with the
/// file, not with the path.
///
/// The meta file is JSON, checked in alongside the source, so a team shares ids
/// through version control.  It is small and human readable on purpose: the
/// alternative (a central database) makes every asset change a merge conflict.

#include "local3d/assets/AssetId.hpp"
#include "local3d/assets/AssetData.hpp"
#include "local3d/assets/FileSystem.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/serialization/Json.hpp"

#include <string>
#include <string_view>

namespace l3d::assets {

/// The sidecar path for a source file: "models/cube.glb" -> "models/cube.glb.l3dmeta".
[[nodiscard]] inline AssetPath MetaPathFor(const AssetPath& source) {
    return source.Appending(kMetaExtension);
}

struct AssetMeta {
    /// Bumped when the on disk layout changes; older files are still read.
    static constexpr u32 kCurrentFormatVersion = 1;

    u32 formatVersion = kCurrentFormatVersion;
    AssetId id;
    AssetType type = AssetType::Unknown;
    /// Importer that produced the cooked data ("glb", "png", "wav").
    std::string importer;
    /// Importer revision.  Bumping it invalidates every cooked file of that
    /// importer without touching the sources.
    u32 importerVersion = 0;
    /// Importer specific, human editable settings.
    serial::JsonValue settings = serial::JsonValue::MakeObject();
    /// Source hash and import fingerprint at the time of the last successful
    /// cook.  Comparing them with the current values is what makes cooking
    /// incremental.
    u64 cookedSourceHash = 0;
    u64 cookedImportHash = 0;

    /// Hash covering the importer, its version and the settings: everything
    /// that would change the import output without changing the source bytes.
    [[nodiscard]] u64 ImportFingerprint() const;

    [[nodiscard]] std::string Dump() const;
    [[nodiscard]] static Result<AssetMeta> Parse(std::string_view jsonText);

    [[nodiscard]] static Result<AssetMeta> Load(IFileSystem& fs, const AssetPath& sourcePath);
    [[nodiscard]] OperationResult Save(IFileSystem& fs, const AssetPath& sourcePath) const;
};

/// The import fingerprint, shared by the meta file and the database record so
/// both compute exactly the same value.
[[nodiscard]] u64 ImportFingerprintOf(std::string_view importer, u32 importerVersion,
                                      u64 settingsHash) noexcept;

/// Stable hash of a settings object.  `Dump(0)` is deterministic because
/// JsonObject is an ordered map, so the same settings always hash the same.
[[nodiscard]] u64 HashSettings(const serial::JsonValue& settings);

/// Typed access to the texture importer's settings block.
[[nodiscard]] TextureImportSettings TextureSettingsFromJson(const serial::JsonValue& settings) noexcept;
[[nodiscard]] serial::JsonValue TextureSettingsToJson(const TextureImportSettings& settings);

} // namespace l3d::assets
