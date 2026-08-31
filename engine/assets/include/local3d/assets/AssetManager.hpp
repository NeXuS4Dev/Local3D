#pragma once
/// @file AssetManager.hpp
/// @brief The runtime side of the pipeline: cooked files in, engine data out.
///
/// The runtime never sees source files.  It reads the cook manifest, and loads
/// cooked assets by id through this class, which decodes them once and caches
/// the result.
///
/// Lifetime rules, because returning pointers into a cache is how asset systems
/// crash:
///   * returned pointers stay valid until `Release(id)` or `ClearCache()`;
///   * loading an asset never invalidates a pointer that was already handed out
///     (entries are held through unique_ptr, so the cache can rehash freely);
///   * nothing is evicted behind your back - there is no budget driven eviction
///     yet, so a long running game must release what it no longer uses.
///
/// Thread safety: none.  Loading touches the file system and the cache; call it
/// from one thread, or pre-load on a worker and hand the manager over.

#include "local3d/assets/AssetData.hpp"
#include "local3d/assets/AssetId.hpp"
#include "local3d/assets/Cooker.hpp"
#include "local3d/assets/FileSystem.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace l3d::assets {

/// One decoded cooked asset.
struct CachedAsset {
    AssetType type = AssetType::Unknown;
    /// Size of the cooked file it came from, for memory accounting.
    u64 cookedBytes = 0;
    std::variant<MeshDocument, TextureDocument, AudioDocument, CookedMaterial> data;
};

class AssetManager {
public:
    /// Reads the manifest.  A missing manifest is an error: a runtime with no
    /// cooked data should fail loudly at start up, not per asset.
    [[nodiscard]] Result<void> Initialize(IFileSystem& cookedFs,
                                          std::string_view manifestPath = kManifestFileName);
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

    [[nodiscard]] const CookedManifest& Manifest() const noexcept { return manifest_; }
    [[nodiscard]] const CookedManifestEntry* Find(AssetId id) const noexcept;
    /// Linear scan over the manifest; used by tools, not by per frame code.
    [[nodiscard]] const CookedManifestEntry* FindBySourcePath(const AssetPath& path) const noexcept;

    /// Typed accessors.  Each one loads and decodes on first use.
    [[nodiscard]] Result<const MeshDocument*> GetMesh(AssetId id);
    [[nodiscard]] Result<const TextureDocument*> GetTexture(AssetId id);
    [[nodiscard]] Result<const AudioDocument*> GetAudio(AssetId id);
    [[nodiscard]] Result<const CookedMaterial*> GetMaterial(AssetId id);

    /// Cooked bytes without decoding or caching - what a streaming audio
    /// backend or a packer wants.
    [[nodiscard]] Result<std::vector<u8>> LoadRaw(AssetId id) const;

    /// Drops a cached asset.  Pointers previously returned for it dangle.
    void Release(AssetId id);
    /// Drops everything cached.
    void ClearCache();

    [[nodiscard]] usize CachedCount() const noexcept { return cache_.size(); }
    [[nodiscard]] u64 CachedCookedBytes() const noexcept;

private:
    [[nodiscard]] Result<CachedAsset*> Acquire(AssetId id);

    IFileSystem* fs_ = nullptr;
    CookedManifest manifest_;
    bool initialized_ = false;
    std::unordered_map<AssetId, std::unique_ptr<CachedAsset>> cache_;
};

} // namespace l3d::assets
