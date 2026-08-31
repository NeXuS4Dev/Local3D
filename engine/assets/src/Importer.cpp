#include "local3d/assets/Importer.hpp"

namespace l3d::assets {

AssetType ImportedAssetType(const ImportedAsset& asset) noexcept {
    if (std::holds_alternative<MeshDocument>(asset)) {
        return AssetType::Mesh;
    }
    if (std::holds_alternative<TextureDocument>(asset)) {
        return AssetType::Texture;
    }
    return AssetType::AudioClip;
}

void ImportLog::Warning(std::string message) {
    warnings_.push_back(std::move(message));
}

void ImporterRegistry::Register(std::unique_ptr<IImporter> importer) {
    if (importer) {
        importers_.push_back(std::move(importer));
    }
}

IImporter* ImporterRegistry::FindFor(const AssetPath& path) const noexcept {
    for (const std::unique_ptr<IImporter>& importer : importers_) {
        if (importer->CanImport(path)) {
            return importer.get();
        }
    }
    return nullptr;
}

ImporterRegistry CreateDefaultImporters() {
    ImporterRegistry registry;
    registry.Register(CreateGlbImporter());
    registry.Register(CreateImageImporter());
    registry.Register(CreateWavImporter());
    return registry;
}

} // namespace l3d::assets
