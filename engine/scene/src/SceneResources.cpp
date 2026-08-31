#include "local3d/scene/SceneResources.hpp"

#include "local3d/assets/GpuUpload.hpp"
#include "local3d/core/Status.hpp"

#include <utility>

namespace l3d::scene {

Result<render::MeshHandle> SceneResources::Mesh(rhi::IDevice& device, render::Renderer& renderer,
                                               assets::AssetManager& assets, assets::AssetId id) {
    if (const auto found = meshes_.find(id); found != meshes_.end()) {
        return found->second.handle;
    }
    if (id.IsNull()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Cannot load a null mesh id"});
    }

    auto loaded = assets.GetMesh(id);
    if (loaded.IsError()) {
        return Unexpected(loaded.Error());
    }
    const assets::MeshDocument* document = *loaded;
    if (document->meshes.empty()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Cooked mesh holds no geometry"});
    }
    const assets::MeshData& data = document->meshes.front();
    if (!data.IsValid()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Cooked mesh geometry is incomplete"});
    }

    auto handle = renderer.RegisterMesh(device, data);
    if (handle.IsError()) {
        return Unexpected(handle.Error());
    }

    MeshEntry entry;
    entry.handle = *handle;
    entry.bounds = data.bounds;
    meshes_.emplace(id, entry);
    return entry.handle;
}

Result<const rhi::ITexture*> SceneResources::Texture(rhi::IDevice& device,
                                                    assets::AssetManager& assets,
                                                    assets::AssetId id) {
    if (const auto found = textures_.find(id); found != textures_.end()) {
        return found->second.get();
    }
    if (id.IsNull()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Cannot load a null texture id"});
    }

    auto loaded = assets.GetTexture(id);
    if (loaded.IsError()) {
        return Unexpected(loaded.Error());
    }
    const assets::TextureDocument* document = *loaded;
    if (!document->image.IsValid()) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Cooked texture holds no pixels"});
    }

    auto texture = assets::UploadTexture(device, document->image, id.ToString());
    if (texture.IsError()) {
        return Unexpected(texture.Error());
    }

    const rhi::ITexture* raw = texture->get();
    textures_.emplace(id, std::move(*texture));
    return raw;
}

ResolvedMesh SceneResources::Resolve(assets::AssetId id) const {
    const auto found = meshes_.find(id);
    if (found == meshes_.end()) {
        return ResolvedMesh{};
    }
    return ResolvedMesh{found->second.handle, found->second.bounds};
}

void SceneResources::Clear() {
    // Order matters for nothing here, but the textures go through the device's
    // deferred deletion queue, so this must happen while the device is alive.
    textures_.clear();
    meshes_.clear();
}

} // namespace l3d::scene
