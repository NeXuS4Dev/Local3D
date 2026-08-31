#include "local3d/assets/AssetManager.hpp"

namespace l3d::assets {

namespace {

[[nodiscard]] ConstByteSpan AsSpan(const std::vector<u8>& bytes) noexcept {
    return std::as_bytes(std::span(bytes.data(), bytes.size()));
}

} // namespace

Result<void> AssetManager::Initialize(IFileSystem& cookedFs, std::string_view manifestPath) {
    auto text = ReadFileAsText(cookedFs, manifestPath);
    if (text.IsError()) {
        return Unexpected(Status{StatusCode::NotFound,
                                 "No cooked manifest; run the cooker before starting"});
    }
    auto manifest = CookedManifest::Parse(*text);
    if (manifest.IsError()) {
        return Unexpected(manifest.Error());
    }
    manifest_ = std::move(*manifest);
    fs_ = &cookedFs;
    initialized_ = true;
    return {};
}

const CookedManifestEntry* AssetManager::Find(AssetId id) const noexcept {
    return manifest_.Find(id);
}

const CookedManifestEntry* AssetManager::FindBySourcePath(const AssetPath& path) const noexcept {
    for (const CookedManifestEntry& entry : manifest_.entries) {
        if (entry.sourcePath == path) {
            return &entry;
        }
    }
    return nullptr;
}

Result<std::vector<u8>> AssetManager::LoadRaw(AssetId id) const {
    if (!initialized_) {
        return Unexpected(Status{StatusCode::NotInitialized, "Asset manager is not initialized"});
    }
    const CookedManifestEntry* entry = manifest_.Find(id);
    if (entry == nullptr) {
        return Unexpected(Status{StatusCode::NotFound, "Asset is not in the cooked manifest"});
    }
    return fs_->ReadFile(entry->cookedPath.Text());
}

Result<CachedAsset*> AssetManager::Acquire(AssetId id) {
    const auto cached = cache_.find(id);
    if (cached != cache_.end()) {
        return cached->second.get();
    }

    const CookedManifestEntry* entry = manifest_.Find(id);
    if (entry == nullptr) {
        return Unexpected(Status{StatusCode::NotFound, "Asset is not in the cooked manifest"});
    }
    auto bytes = LoadRaw(id);
    if (bytes.IsError()) {
        return Unexpected(bytes.Error());
    }
    const ConstByteSpan cooked = AsSpan(*bytes);

    auto asset = std::make_unique<CachedAsset>();
    asset->type = entry->type;
    asset->cookedBytes = static_cast<u64>(bytes->size());
    switch (entry->type) {
        case AssetType::Mesh: {
            auto document = ReadMeshDocument(cooked);
            if (document.IsError()) {
                return Unexpected(document.Error());
            }
            asset->data = std::move(*document);
            break;
        }
        case AssetType::Texture: {
            auto document = ReadTextureDocument(cooked);
            if (document.IsError()) {
                return Unexpected(document.Error());
            }
            asset->data = std::move(*document);
            break;
        }
        case AssetType::AudioClip: {
            auto document = ReadAudioDocument(cooked);
            if (document.IsError()) {
                return Unexpected(document.Error());
            }
            asset->data = std::move(*document);
            break;
        }
        case AssetType::Material: {
            auto material = ReadMaterial(cooked);
            if (material.IsError()) {
                return Unexpected(material.Error());
            }
            asset->data = std::move(*material);
            break;
        }
        default:
            return Unexpected(
                Status{StatusCode::Unsupported, "This asset type cannot be loaded at runtime"});
    }

    CachedAsset* raw = asset.get();
    cache_.emplace(id, std::move(asset));
    return raw;
}

Result<const MeshDocument*> AssetManager::GetMesh(AssetId id) {
    auto asset = Acquire(id);
    if (asset.IsError()) {
        return Unexpected(asset.Error());
    }
    const MeshDocument* document = std::get_if<MeshDocument>(&(*asset)->data);
    if (document == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Asset is not a mesh"});
    }
    return document;
}

Result<const TextureDocument*> AssetManager::GetTexture(AssetId id) {
    auto asset = Acquire(id);
    if (asset.IsError()) {
        return Unexpected(asset.Error());
    }
    const TextureDocument* document = std::get_if<TextureDocument>(&(*asset)->data);
    if (document == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Asset is not a texture"});
    }
    return document;
}

Result<const AudioDocument*> AssetManager::GetAudio(AssetId id) {
    auto asset = Acquire(id);
    if (asset.IsError()) {
        return Unexpected(asset.Error());
    }
    const AudioDocument* document = std::get_if<AudioDocument>(&(*asset)->data);
    if (document == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Asset is not an audio clip"});
    }
    return document;
}

Result<const CookedMaterial*> AssetManager::GetMaterial(AssetId id) {
    auto asset = Acquire(id);
    if (asset.IsError()) {
        return Unexpected(asset.Error());
    }
    const CookedMaterial* material = std::get_if<CookedMaterial>(&(*asset)->data);
    if (material == nullptr) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Asset is not a material"});
    }
    return material;
}

void AssetManager::Release(AssetId id) {
    cache_.erase(id);
}

void AssetManager::ClearCache() {
    cache_.clear();
}

u64 AssetManager::CachedCookedBytes() const noexcept {
    u64 total = 0;
    for (const auto& [unused, asset] : cache_) {
        total += asset->cookedBytes;
    }
    return total;
}

} // namespace l3d::assets
