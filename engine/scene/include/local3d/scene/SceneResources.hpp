#pragma once
/// @file SceneResources.hpp
/// @brief The asset id -> GPU resource cache the scene graph renders through.
///
/// Scenes reference assets by id (see docs/architecture/assets.md) because ids
/// survive renames and mean the same thing in every run.  The renderer works in
/// mesh handles and texture pointers.  This class is the only place those two
/// worlds meet, which is what keeps the scene graph free of any device
/// dependency and lets a headless test resolve ids to fake handles.
///
/// Lifetime: the GPU objects here are released through the device that created
/// them, so **the device must outlive this object**.  Destroying a
/// SceneResources after its device is a use after free, in exactly the way
/// destroying any GPU resource late is.

#include "local3d/assets/AssetManager.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/renderer/Renderer.hpp"
#include "local3d/rhi/RhiResources.hpp"
#include "local3d/scene/Scene.hpp"

#include <unordered_map>

namespace l3d::scene {

/// Loads cooked assets onto the GPU once and hands out stable handles.
///
/// Not thread safe: it mutates two caches.  Pre-load on one thread before the
/// frame starts, which is what a loading screen is for.
class SceneResources final : public IMeshResolver {
public:
    /// Uploads the mesh behind `id` if it is not loaded yet.  A cooked mesh file
    /// holds exactly one mesh - the cooker splits multi mesh documents - so the
    /// first entry is the only one.
    [[nodiscard]] Result<render::MeshHandle> Mesh(rhi::IDevice& device, render::Renderer& renderer,
                                                 assets::AssetManager& assets, assets::AssetId id);

    /// Uploads a texture and its mip chain once.  The returned pointer stays
    /// valid until Clear() or until this object is destroyed.
    [[nodiscard]] Result<const rhi::ITexture*> Texture(rhi::IDevice& device,
                                                      assets::AssetManager& assets,
                                                      assets::AssetId id);

    /// IMeshResolver: what CollectDrawItems asks for.  Never loads anything -
    /// collecting draw items must not touch the device mid frame.
    [[nodiscard]] ResolvedMesh Resolve(assets::AssetId id) const override;

    [[nodiscard]] usize MeshCount() const noexcept { return meshes_.size(); }
    [[nodiscard]] usize TextureCount() const noexcept { return textures_.size(); }
    [[nodiscard]] bool IsMeshLoaded(assets::AssetId id) const noexcept {
        return meshes_.contains(id);
    }

    /// Releases every GPU object.  Every handle and pointer handed out before
    /// this dangles.
    void Clear();

private:
    struct MeshEntry {
        render::MeshHandle handle = render::kInvalidMesh;
        math::Aabb bounds;
    };

    std::unordered_map<assets::AssetId, MeshEntry> meshes_;
    std::unordered_map<assets::AssetId, rhi::TexturePtr> textures_;
};

} // namespace l3d::scene
