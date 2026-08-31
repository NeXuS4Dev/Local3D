#pragma once
/// @file FrameView.hpp
/// @brief A camera for one frame: matrices, frustum and viewport.

#include "local3d/math/Geometry.hpp"
#include "local3d/math/Matrix.hpp"
#include "local3d/math/Vector.hpp"
#include "local3d/renderer/RenderTypes.hpp"

#include <span>
#include <vector>

namespace l3d::render {

struct FrameView {
    math::Vec3 position{};
    math::Vec3 target{};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    f32 fovYDegrees = 60.0f;
    f32 aspect = 16.0f / 9.0f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 500.0f;
    u32 viewportWidth = 1280;
    u32 viewportHeight = 720;

    math::Mat4 view = math::Mat4::Identity();
    math::Mat4 projection = math::Mat4::Identity();
    math::Mat4 viewProjection = math::Mat4::Identity();
    math::Frustum frustum{};

    /// Fills view/projection/viewProjection/frustum from the camera parameters.
    void Update() noexcept;

    [[nodiscard]] rhi::Viewport Viewport() const noexcept;
    /// Fraction of the viewport height a sphere of `radius` at `distance` covers.
    [[nodiscard]] f32 ScreenCoverage(f32 radius, f32 distance) const noexcept;
};

/// What culling decided.
struct CullResult {
    /// Indices into the input draw item list, in input order.
    std::vector<u32> visible;
    u32 tested = 0;
    u32 frustumCulled = 0;
    /// lod index chosen per visible item (parallel to `visible`).
    std::vector<u8> lodIndex;
    std::array<u32, 4> lodHistogram{};
};

/// Frustum cull and select lods.  Deterministic: the output order follows the
/// input order, so batching is stable frame to frame.
void CullDrawItems(const FrameView& view, std::span<const DrawItem> items, CullResult& out);

/// Choose the lod for one item given its screen coverage.
[[nodiscard]] u8 SelectLod(const DrawItem& item, f32 coverage) noexcept;

/// View space cascade split distances, ascending, ending at the shadow distance.
[[nodiscard]] std::vector<f32> ComputeCascadeSplits(f32 nearPlane, f32 farPlane,
                                                    const ShadowSettings& settings) noexcept;

/// Build one cascade: the light space matrix covering the camera sub-frustum
/// between `nearSlice` and `farSlice`.
[[nodiscard]] ShadowCascade ComputeCascade(const FrameView& view, math::Vec3 lightDirection,
                                           f32 nearSlice, f32 farSlice,
                                           const ShadowSettings& settings) noexcept;

/// The eight view space corners of a camera sub-frustum.  Exposed for tests and
/// for the editor's cascade debug overlay.
[[nodiscard]] std::array<math::Vec3, 8> ViewSpaceFrustumCorners(const FrameView& view,
                                                                f32 nearSlice,
                                                                f32 farSlice) noexcept;

} // namespace l3d::render
