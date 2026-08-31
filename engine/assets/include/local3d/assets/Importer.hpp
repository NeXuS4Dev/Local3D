#pragma once
/// @file Importer.hpp
/// @brief The importer interface and the registry that picks one per file.
///
/// An importer turns opaque source bytes into engine data (see AssetData.hpp).
/// It is deliberately ignorant of ids, of the file system and of the GPU: it
/// gets bytes and settings and returns data, which keeps importers trivially
/// testable and lets the cooker run them on a worker thread.
///
/// Importers must be deterministic.  The same bytes and settings always produce
/// the same output, because the cook hash is what decides whether work is
/// skipped - a non deterministic importer would silently serve stale assets.

#include "local3d/assets/AssetData.hpp"
#include "local3d/assets/AssetId.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/serialization/Json.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace l3d::assets {

/// What came out of an import.  One source file produces one document, but a
/// document can hold many sub-assets (a GLB holds meshes and materials).
using ImportedAsset = std::variant<MeshDocument, TextureDocument, AudioDocument>;

[[nodiscard]] AssetType ImportedAssetType(const ImportedAsset& asset) noexcept;

/// Non fatal problems found during an import.  Importers keep going wherever
/// they can; the editor shows these next to the asset in the asset browser.
class ImportLog {
public:
    void Warning(std::string message);
    [[nodiscard]] const std::vector<std::string>& Warnings() const noexcept { return warnings_; }
    [[nodiscard]] usize WarningCount() const noexcept { return warnings_.size(); }
    void Clear() { warnings_.clear(); }

private:
    std::vector<std::string> warnings_;
};

class IImporter {
public:
    virtual ~IImporter() = default;

    /// Stable name recorded in the meta file, e.g. "glb".
    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
    /// Bumped when the importer's output changes; invalidates cooked data.
    [[nodiscard]] virtual u32 Version() const noexcept = 0;
    [[nodiscard]] virtual AssetType OutputType() const noexcept = 0;
    [[nodiscard]] virtual bool CanImport(const AssetPath& path) const noexcept = 0;

    [[nodiscard]] virtual Result<ImportedAsset> Import(ConstByteSpan sourceBytes,
                                                      const serial::JsonValue& settings,
                                                      const AssetPath& sourcePath,
                                                      ImportLog& log) = 0;
};

[[nodiscard]] std::unique_ptr<IImporter> CreateGlbImporter();
/// PNG/JPEG/TGA/BMP through stb_image (see docs/architecture/dependencies.md).
[[nodiscard]] std::unique_ptr<IImporter> CreateImageImporter();
[[nodiscard]] std::unique_ptr<IImporter> CreateWavImporter();

/// Chooses an importer for a path.  Registration order breaks ties.
class ImporterRegistry {
public:
    void Register(std::unique_ptr<IImporter> importer);
    [[nodiscard]] IImporter* FindFor(const AssetPath& path) const noexcept;
    [[nodiscard]] usize Count() const noexcept { return importers_.size(); }

private:
    std::vector<std::unique_ptr<IImporter>> importers_;
};

/// The engine's built in importers: GLB, images, WAV.
[[nodiscard]] ImporterRegistry CreateDefaultImporters();

} // namespace l3d::assets
