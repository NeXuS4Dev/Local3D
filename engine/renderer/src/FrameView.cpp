#include "local3d/renderer/FrameView.hpp"

#include "local3d/math/Constants.hpp"

#include <algorithm>
#include <limits>
#include <cmath>

namespace l3d::render {
namespace {

[[nodiscard]] constexpr f32 ClampF32(f32 value, f32 min, f32 max) noexcept {
    return value < min ? min : (value > max ? max : value);
}

} // namespace

void FrameView::Update() noexcept {
    view = math::Mat4::LookAtRH(position, target, up);
    projection = math::Mat4::PerspectiveRH(fovYDegrees, aspect, nearPlane, farPlane);
    viewProjection = projection * view;
    frustum = math::Frustum::FromViewProjection(viewProjection);
}

rhi::Viewport FrameView::Viewport() const noexcept {
    rhi::Viewport viewport;
    viewport.width = static_cast<f32>(viewportWidth);
    viewport.height = static_cast<f32>(viewportHeight);
    return viewport;
}

f32 FrameView::ScreenCoverage(f32 radius, f32 distance) const noexcept {
    if (distance <= 0.0f) {
        return 1.0f;
    }
    // Height of the viewport in world units at `distance`, then the object's
    // projected height as a fraction of it.
    const f32 halfFov = (fovYDegrees * math::kDegToRad) * 0.5f;
    const f32 tanHalfFov = std::tan(halfFov);
    if (tanHalfFov <= 0.0f) {
        return 1.0f;
    }
    const f32 worldHeight = 2.0f * distance * tanHalfFov;
    return (2.0f * radius) / worldHeight;
}

std::array<math::Vec3, 8> ViewSpaceFrustumCorners(const FrameView& view, f32 nearSlice,
                                                  f32 farSlice) noexcept {
    // The camera looks down -Z, so the horizontal half extent at depth d is
    // d * tan(fovX/2) and the vertical one is d * tan(fovY/2).
    const f32 tanHalfY = std::tan((view.fovYDegrees * math::kDegToRad) * 0.5f);
    const f32 tanHalfX = tanHalfY * view.aspect;

    std::array<math::Vec3, 8> corners{};
    const f32 depths[2] = {-nearSlice, -farSlice};
    usize index = 0;
    for (const f32 depth : depths) {
        const f32 halfX = -depth * tanHalfX;
        const f32 halfY = -depth * tanHalfY;
        corners[index++] = math::Vec3{-halfX, -halfY, depth};
        corners[index++] = math::Vec3{halfX, -halfY, depth};
        corners[index++] = math::Vec3{halfX, halfY, depth};
        corners[index++] = math::Vec3{-halfX, halfY, depth};
    }
    return corners;
}

u8 SelectLod(const DrawItem& item, f32 coverage) noexcept {
    // The switch thresholds pick the *wanted* level; coverage above the first
    // threshold means lod 0, below all of them means the coarsest.
    usize wanted = item.lodSwitchCoverage.size();
    for (usize lod = 0; lod < item.lodSwitchCoverage.size(); ++lod) {
        if (coverage >= item.lodSwitchCoverage[lod]) {
            wanted = lod;
            break;
        }
    }
    if (wanted < item.lods.size() && item.lods[wanted] != kInvalidMesh) {
        return static_cast<u8>(wanted);
    }
    // Chains are often only partially authored (a two-lod prop still has three
    // empty slots).  Fall back to the nearest finer lod, then the nearest
    // coarser one, so a missing level degrades quality instead of dropping the
    // draw entirely.
    for (usize lod = wanted; lod-- > 0;) {
        if (item.lods[lod] != kInvalidMesh) {
            return static_cast<u8>(lod);
        }
    }
    for (usize lod = wanted + 1; lod < item.lods.size(); ++lod) {
        if (item.lods[lod] != kInvalidMesh) {
            return static_cast<u8>(lod);
        }
    }
    return 0;
}

void CullDrawItems(const FrameView& view, std::span<const DrawItem> items, CullResult& out) {
    out.visible.clear();
    out.lodIndex.clear();
    out.lodHistogram = {};
    out.tested = static_cast<u32>(items.size());
    out.frustumCulled = 0;

    for (u32 i = 0; i < items.size(); ++i) {
        const DrawItem& item = items[i];
        // Cull in world space: the bounds are authored in local space, so the
        // world matrix is applied first.
        const math::Aabb worldBounds = item.localBounds.Transformed(item.world);
        const math::Frustum::Test test = view.frustum.TestAabb(worldBounds);
        if (test == math::Frustum::Test::Outside) {
            ++out.frustumCulled;
            continue;
        }
        const math::Vec3 center = worldBounds.Center();
        const f32 distance = math::Length(center - view.position);
        const f32 radius = math::Length(worldBounds.Extents());
        const f32 coverage = view.ScreenCoverage(radius, distance);
        const u8 lod = SelectLod(item, coverage);
        out.visible.push_back(i);
        out.lodIndex.push_back(lod);
        if (lod < out.lodHistogram.size()) {
            ++out.lodHistogram[lod];
        }
    }
}

std::vector<f32> ComputeCascadeSplits(f32 nearPlane, f32 farPlane,
                                      const ShadowSettings& settings) noexcept {
    const u32 count = settings.ClampedCascadeCount();
    const f32 shadowFar = ClampF32(settings.maxDistance, nearPlane, farPlane);

    std::vector<f32> splits;
    splits.reserve(count);
    const f32 lambda = ClampF32(settings.lambda, 0.0f, 1.0f);
    for (u32 i = 1; i <= count; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(count);
        // Logarithmic split keeps relative resolution constant with distance.
        const f32 logSplit = nearPlane * std::pow(shadowFar / nearPlane, t);
        const f32 linearSplit = nearPlane + (shadowFar - nearPlane) * t;
        splits.push_back(lambda * logSplit + (1.0f - lambda) * linearSplit);
    }
    // Guard against rounding pushing the last split past the shadow distance.
    if (!splits.empty()) {
        splits.back() = shadowFar;
    }
    return splits;
}

ShadowCascade ComputeCascade(const FrameView& view, math::Vec3 lightDirection, f32 nearSlice,
                             f32 farSlice, const ShadowSettings& settings) noexcept {
    ShadowCascade cascade;
    const math::Vec3 lightDir = math::LengthSquared(lightDirection) > 0.0f
                                    ? math::Normalize(lightDirection)
                                    : math::Vec3{0.0f, -1.0f, 0.0f};

    // 1. Corners of the camera sub-frustum, in world space.
    const std::array<math::Vec3, 8> viewCorners =
        ViewSpaceFrustumCorners(view, nearSlice, farSlice);
    const math::Mat4 inverseView = view.view.Inverse();
    math::Vec3 center{};
    for (const math::Vec3& corner : viewCorners) {
        center = center + inverseView.TransformPoint(corner);
    }
    center = center * (1.0f / 8.0f);

    // 2. Light space basis: the light looks at the frustum centre from far enough
    //    away that the whole slice is in front of it.
    f32 radius = 0.0f;
    for (const math::Vec3& corner : viewCorners) {
        const math::Vec3 world = inverseView.TransformPoint(corner);
        radius = std::max(radius, math::Length(world - center));
    }
    const math::Vec3 lightOrigin = center - lightDir * (radius + 1.0f);
    // A sun pointing straight down is parallel to the world up vector, which
    // leaves LookAt with a degenerate basis (and produces NaNs).  Switch to the
    // Z axis as up whenever the light is close to vertical.
    const math::Vec3 up = std::abs(lightDir.y) > 0.99f ? math::Vec3{0.0f, 0.0f, 1.0f}
                                                       : math::Vec3{0.0f, 1.0f, 0.0f};
    const math::Mat4 lightView = math::Mat4::LookAtRH(lightOrigin, center, up);

    // 3. Tight ortho box around the corners in light space.
    f32 minX = std::numeric_limits<f32>::max();
    f32 maxX = std::numeric_limits<f32>::lowest();
    f32 minY = minX;
    f32 maxY = maxX;
    f32 minZ = minX;
    f32 maxZ = maxX;
    for (const math::Vec3& corner : viewCorners) {
        const math::Vec3 lightSpace = lightView.TransformPoint(inverseView.TransformPoint(corner));
        minX = std::min(minX, lightSpace.x);
        maxX = std::max(maxX, lightSpace.x);
        minY = std::min(minY, lightSpace.y);
        maxY = std::max(maxY, lightSpace.y);
        minZ = std::min(minZ, lightSpace.z);
        maxZ = std::max(maxZ, lightSpace.z);
    }

    const f32 mapSize = static_cast<f32>(settings.mapSize > 0 ? settings.mapSize : 1);
    // A square box: cascades are rendered into a square atlas slice, and a square
    // keeps the texel size identical on both axes.
    const f32 halfWidth = std::max(maxX - minX, maxY - minY) * 0.5f;
    f32 texelSize = (2.0f * halfWidth) / mapSize;
    if (texelSize > 0.0f) {
        const f32 centerX = (minX + maxX) * 0.5f;
        const f32 centerY = (minY + maxY) * 0.5f;
        f32 snappedX = centerX;
        f32 snappedY = centerY;
        if (settings.texelSnapping) {
            // Round the box centre to the texel grid.  Snapping the centre rather
            // than each bound is what keeps the box a whole number of texels wide
            // while still covering the slice.
            snappedX = std::floor(centerX / texelSize + 0.5f) * texelSize;
            snappedY = std::floor(centerY / texelSize + 0.5f) * texelSize;
        }
        // Round the box radius up so no corner is left outside it.
        const f32 boxRadius = std::ceil(halfWidth / texelSize) * texelSize;
        minX = snappedX - boxRadius;
        maxX = snappedX + boxRadius;
        minY = snappedY - boxRadius;
        maxY = snappedY + boxRadius;
        texelSize = (2.0f * boxRadius) / mapSize;
    }
    cascade.texelWorldSize = texelSize;

    cascade.viewProjection =
        math::Mat4::OrthographicRH(minX, maxX, minY, maxY, -maxZ - radius * 2.0f, -minZ + radius) *
        lightView;
    cascade.splitViewDepth = farSlice;
    return cascade;
}

} // namespace l3d::render
