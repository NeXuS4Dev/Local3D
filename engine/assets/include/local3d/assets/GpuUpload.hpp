#pragma once
/// @file GpuUpload.hpp
/// @brief The bridge from cooked asset data to GPU resources.
///
/// Kept separate from AssetManager so the pure data path (load, decode, cache)
/// can be used headlessly - by tests, by the cooker's validation, by a dedicated
/// server - without dragging a device in.

#include "local3d/assets/AssetData.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/rhi/RhiDevice.hpp"
#include "local3d/rhi/RhiResources.hpp"

#include <string_view>

namespace l3d::assets {

/// Creates a sampled texture and uploads every mip.
///
/// The mip data is handed to the device as one span per level, which is the
/// shape both the null and the Vulkan backends want: no staging buffer
/// bookkeeping leaks into the asset layer.
[[nodiscard]] Result<rhi::TexturePtr> UploadTexture(rhi::IDevice& device, const ImageData& image,
                                                    std::string_view debugName = {});

} // namespace l3d::assets
