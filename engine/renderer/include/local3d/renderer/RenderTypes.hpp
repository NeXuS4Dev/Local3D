#pragma once
/// @file RenderTypes.hpp
/// @brief Frame level data: views, draw items, lights, settings and statistics.
///
/// These are the inputs the renderer is given each frame.  They are plain data
/// so a scene system, an editor viewport and a headless test can all produce
/// them without depending on each other.

#include "local3d/core/Common.hpp"
#include "local3d/math/Geometry.hpp"
#include "local3d/math/Matrix.hpp"
#include "local3d/math/Vector.hpp"
#include "local3d/rhi/RhiTypes.hpp"

#include <array>
#include <string>
#include <vector>

namespace l3d::rhi {
class ITexture;
}

namespace l3d::render {

/// Maximum cascaded shadow maps.  Four is the usual quality/fit trade off.
inline constexpr u32 kMaxShadowCascades = 4;

/// One instance as the GPU sees it.  The layout must match the vertex shader's
/// instance attributes; changing it is a shader and renderer change together.
struct GpuInstanceData {
    math::Mat4 world;
    math::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(GpuInstanceData) == 80, "Instance layout must match the shader");

/// A mesh registered with the renderer.
using MeshHandle = u32;
inline constexpr MeshHandle kInvalidMesh = 0xFFFF'FFFF;

/// CPU side geometry.  The asset pipeline fills this in; the renderer uploads it.
struct MeshData {
    std::string name;
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;
    std::vector<math::Vec2> uvs;
    std::vector<u32> indices;
    math::Aabb bounds;

    [[nodiscard]] bool IsValid() const noexcept {
        return !positions.empty() && positions.size() == normals.size() &&
               positions.size() == uvs.size() && indices.size() >= 3 &&
               indices.size() % 3 == 0;
    }
};

/// A renderable.  `lods` is ordered from most to least detailed; the renderer
/// picks one by screen coverage.
struct DrawItem {
    std::array<MeshHandle, 4> lods{kInvalidMesh, kInvalidMesh, kInvalidMesh, kInvalidMesh};
    /// Screen coverage (fraction of the viewport height) below which the next
    /// lod is used.  Size 3 for 4 lods.
    std::array<f32, 3> lodSwitchCoverage{0.35f, 0.12f, 0.04f};
    math::Mat4 world = math::Mat4::Identity();
    math::Aabb localBounds;
    math::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    bool castsShadow = true;
};

/// A directional light.  Point/spot lights arrive with the lighting rework; the
/// shadow code here is cascade specific.
struct DirectionalLight {
    math::Vec3 direction{0.0f, -1.0f, 0.0f}; ///< Direction the light travels.
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    bool castsShadow = true;
};

/// Image based lighting inputs.  Irradiance/prefiltered maps are produced by the
/// asset pipeline; the renderer only binds them.
struct IblParameters {
    const rhi::ITexture* irradianceMap = nullptr;
    const rhi::ITexture* prefilteredMap = nullptr;
    const rhi::ITexture* brdfLut = nullptr;
    f32 intensity = 1.0f;
};

/// One cascade of a cascaded shadow map.
struct ShadowCascade {
    math::Mat4 viewProjection = math::Mat4::Identity();
    /// View space distance at which this cascade ends.
    f32 splitViewDepth = 0.0f;
    /// World units per shadow texel, used to fade cascades and debug overlays.
    f32 texelWorldSize = 0.0f;
};

struct ShadowSettings {
    u32 cascadeCount = 4;
    u32 mapSize = 1024;
    /// Shadows are only rendered inside this distance from the camera.
    f32 maxDistance = 120.0f;
    /// 0 = linear splits, 1 = fully logarithmic.  Logarithmic gives more
    /// resolution near the camera, which is where it is visible.
    f32 lambda = 0.6f;
    /// Snap the light matrix to the texel grid so the shadow map does not shimmer
    /// as the camera moves.
    bool texelSnapping = true;

    [[nodiscard]] u32 ClampedCascadeCount() const noexcept {
        return cascadeCount == 0 ? 1 : (cascadeCount > kMaxShadowCascades ? kMaxShadowCascades
                                                                          : cascadeCount);
    }
};

/// What a frame cost, for the profiler and the editor's stats overlay.
struct RendererStats {
    u32 drawItems = 0;
    u32 visibleItems = 0;
    u32 frustumCulled = 0;
    u32 shadowCasters = 0;
    u32 batches = 0;
    u32 instances = 0;
    u32 drawCalls = 0;
    u32 dispatches = 0;
    u32 graphPasses = 0;
    u32 graphCulledPasses = 0;
    u64 transientBytes = 0;
    std::array<u32, 4> lodHistogram{};
};

} // namespace l3d::render
