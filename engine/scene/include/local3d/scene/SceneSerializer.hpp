#pragma once
/// @file SceneSerializer.hpp
/// @brief Scene files: JSON, one entry per node, references by array index.
///
/// The format is deliberately boring.  JSON because a scene is small, edited by
/// hand and reviewed in a pull request; a flat node array because a nested tree
/// cannot express a node whose parent was created after it (entity indices are
/// recycled, so a child can have a lower index than its parent); indices rather
/// than entity handles because handles are only meaningful inside one world.
///
/// Asset references are asset ids, never paths, so renaming a source file does
/// not break a scene - see docs/architecture/assets.md.

#include "local3d/core/Result.hpp"
#include "local3d/scene/Scene.hpp"

#include <string>
#include <string_view>

namespace l3d::scene {

struct SceneSerializer {
    /// Bumped when the layout changes in a way an older engine cannot read.
    static constexpr u32 kFormatVersion = 1;

    /// Nodes are written in Scene::Nodes() order (entity index order), which is
    /// stable, so saving an unchanged scene produces a byte identical file.
    [[nodiscard]] static Result<std::string> ToJson(const Scene& scene, u32 indent = 2);

    /// Rebuilds a scene.  `out` is cleared first.  A file written by a newer
    /// engine is an error rather than a partially loaded scene.
    [[nodiscard]] static Result<void> FromJson(std::string_view text, Scene& out);
};

} // namespace l3d::scene
