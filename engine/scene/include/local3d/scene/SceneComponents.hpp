#pragma once
/// @file SceneComponents.hpp
/// @brief The components a 3D scene is made of.
///
/// Plain data, no behaviour: the logic that needs the whole graph (world
/// matrices, parenting, frame inputs) lives in Scene, because a component
/// cannot see its siblings and half of these questions are about siblings.
///
/// One rule to keep in mind when adding a component: `TransformComponent` is
/// what makes an entity a *node*.  Every other component here is meaningless
/// without one, because they are all positioned by it.

#include "local3d/assets/AssetId.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/ecs/Entity.hpp"
#include "local3d/math/Color.hpp"
#include "local3d/math/Matrix.hpp"
#include "local3d/math/Transform.hpp"

#include <array>
#include <string>
#include <vector>

namespace l3d::scene {

/// Display name.  Empty is legal ("Node 7" is the editor's fallback), so this
/// is never a lookup key - use the entity handle for that.
struct NameComponent {
    std::string name;
};

/// Where a node sits in the hierarchy.
///
/// `children` is part of the component rather than a side table in Scene so the
/// graph has exactly one representation, but it is mutated only through
/// Scene::SetParent / Scene::CreateNode / Scene::DestroyNode.  Editing it by
/// hand desynchronises the parent links, which is why there is no public
/// accessor that hands out a mutable reference.
///
/// Dirty tracking, and why both directions are marked: `Scene::UpdateTransforms`
/// walks down from the roots and prunes a subtree as soon as it sees a clean
/// node.  That is only sound if a dirty node always has a dirty parent, so
/// invalidating a node marks its ancestors as well as its descendants.  See
/// docs/architecture/scene.md.
struct TransformComponent {
    math::Transform local;
    ecs::Entity parent = ecs::kNullEntity;
    /// Children in creation order, which is what the editor's outliner shows.
    std::vector<ecs::Entity> children;
    /// World matrix as of the last UpdateTransforms that reached this node.
    math::Mat4 world = math::Mat4::Identity();
    /// True when `world` no longer matches the local transforms above it.
    bool dirty = true;

    [[nodiscard]] bool IsRoot() const noexcept { return !parent.IsValid(); }
};

/// Draws a mesh.  Lod slots run from most to least detailed and are asset ids,
/// never handles: the handles come from the resource cache at run time, so a
/// scene file stays valid across runs and across devices.
struct MeshRendererComponent {
    static constexpr u32 kMaxLods = 4;

    /// Most detailed first.  Trailing nulls mean "no coarser lod authored",
    /// which the renderer handles by falling back to the nearest finer one.
    std::array<assets::AssetId, kMaxLods> lods{};
    /// Screen coverage below which the next lod is used; size kMaxLods - 1.
    std::array<f32, kMaxLods - 1> lodSwitchCoverage{0.35f, 0.12f, 0.04f};
    /// Material asset.  Null means "the material that came with the mesh".
    assets::AssetId material;
    /// Multiplied into the material's base colour; also what the editor tints a
    /// selected object with.
    math::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    bool castsShadow = true;
    bool visible = true;

    [[nodiscard]] bool HasMesh() const noexcept { return !lods[0].IsNull(); }
    [[nodiscard]] u32 LodCount() const noexcept {
        u32 count = 0;
        for (const assets::AssetId& lod : lods) {
            if (lod.IsNull()) {
                break;
            }
            ++count;
        }
        return count;
    }
};

/// A light.  The renderer currently consumes only the directional one (the
/// shadow code is cascade specific); point and spot are stored and reported so
/// the editor and the lighting rework have something to work with.
struct LightComponent {
    enum class Type : u8 {
        Directional = 0,
        Point,
        Spot,
    };

    Type type = Type::Directional;
    /// Linear space, unbounded: a 5000 lumen lamp is a large number here, and
    /// tonemapping is what brings it into range.
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    /// Point/spot falloff distance in world units.  Ignored for directional.
    f32 range = 10.0f;
    f32 innerAngleDegrees = 25.0f;
    f32 outerAngleDegrees = 35.0f;
    bool castsShadow = true;

    /// Direction the light travels, for a directional light.  A light node
    /// points where it shines: the node's -Z axis, matching the camera
    /// convention, so "look at" a target works for both.
    [[nodiscard]] math::Vec3 DirectionFromWorld(const math::Mat4& world) const noexcept {
        if (type != Type::Directional) {
            return math::Vec3::Down();
        }
        // Normalize() returns the zero vector for degenerate input rather than
        // NaN, so a zero scaled light node cannot poison the shadow matrix.
        return math::Normalize(world.TransformDirection(math::Vec3::Forward()));
    }
};

/// A camera.  Perspective only: Local3D is a 3D engine and an orthographic
/// projection is a viewport option, not a different camera.
struct CameraComponent {
    f32 fovYDegrees = 60.0f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 500.0f;
    math::Color clearColor{0.05f, 0.06f, 0.09f, 1.0f};
    /// Which camera renders when nobody asks for one by name.  Exactly one node
    /// should have this set; Scene::SetActiveCamera enforces that.
    bool active = false;

    [[nodiscard]] f32 AspectFor(u32 width, u32 height) const noexcept {
        return height == 0 ? 1.0f : static_cast<f32>(width) / static_cast<f32>(height);
    }

    /// Rejects the values that produce a NaN or an inverted projection, which is
    /// otherwise a black screen with no explanation.
    [[nodiscard]] bool IsValid() const noexcept {
        return fovYDegrees > 0.0f && fovYDegrees < 180.0f && nearPlane > 0.0f &&
               farPlane > nearPlane;
    }
};

} // namespace l3d::scene
