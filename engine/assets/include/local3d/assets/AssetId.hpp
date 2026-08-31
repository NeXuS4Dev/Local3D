#pragma once
/// @file AssetId.hpp
/// @brief Asset identity: the id, the path and the type.
///
/// Two different questions get two different answers here, and conflating them
/// is what makes asset systems break on rename:
///
///   * **AssetId** is permanent.  It is stored in a sidecar `.l3dmeta` file next
///     to the source, so moving or renaming a file carries its id with it, and
///     every reference in scenes, prefabs and materials keeps working.  Ids are
///     random for user content and derived from a name for engine built-ins.
///   * **AssetPath** is a lookup key, not an identity.  It is normalised, root
///     relative and case insensitive (the platforms we ship to are), which makes
///     it usable in maps and in the editor's asset browser but never stored as a
///     reference.
///
/// See docs/architecture/assets.md.

#include "local3d/core/Common.hpp"
#include "local3d/core/Hash.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/core/Uuid.hpp"

#include <string>
#include <string_view>

namespace l3d::assets {

/// A permanent reference to an asset.  Strong type over Uuid so an id can never
/// be confused with a hash or a handle.
struct AssetId {
    Uuid uuid;

    [[nodiscard]] static AssetId Generate() noexcept { return AssetId{Uuid::Generate()}; }

    /// Deterministic id for engine built-ins ("builtin/white", "builtin/missing").
    [[nodiscard]] static constexpr AssetId FromName(std::string_view name) noexcept {
        return AssetId{Uuid::FromName(name)};
    }

    /// Parses the canonical textual form.
    [[nodiscard]] static Result<AssetId> Parse(std::string_view text) noexcept;

    /// Deterministic id for a sub-asset inside a container (mesh 2 of a GLB).
    /// Two sub-assets of the same source never collide and the mapping is stable
    /// across processes, which is what lets cooked files be named by id.
    [[nodiscard]] constexpr AssetId Sub(u32 index) const noexcept {
        return AssetId{Uuid{MixHash(uuid.High() ^ (static_cast<u64>(index) << 32)),
                            MixHash(uuid.Low() ^ index)}};
    }

    [[nodiscard]] constexpr bool IsNull() const noexcept { return uuid.IsNull(); }
    [[nodiscard]] std::string ToString() const { return uuid.ToString(); }
    [[nodiscard]] constexpr u64 Hash() const noexcept { return uuid.Hash(); }

    friend constexpr bool operator==(const AssetId& a, const AssetId& b) noexcept {
        return a.uuid == b.uuid;
    }
    friend constexpr bool operator!=(const AssetId& a, const AssetId& b) noexcept {
        return !(a == b);
    }
    friend constexpr auto operator<=>(const AssetId& a, const AssetId& b) noexcept {
        return a.uuid <=> b.uuid;
    }
};

inline constexpr AssetId kNullAssetId{};

[[nodiscard]] std::string to_string(const AssetId& id);

/// What kind of thing an asset is.  Determines the importer and the cooked
/// format, so it is persisted in the meta file rather than re-derived from the
/// extension on every load.
enum class AssetType : u8 {
    Unknown = 0,
    Mesh,
    Texture,
    Material,
    AudioClip,
    AnimationClip,
    Scene,
    Prefab,
    Shader,
    Count,
};

[[nodiscard]] constexpr std::string_view AssetTypeToString(AssetType type) noexcept {
    switch (type) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Texture: return "Texture";
        case AssetType::Material: return "Material";
        case AssetType::AudioClip: return "AudioClip";
        case AssetType::AnimationClip: return "AnimationClip";
        case AssetType::Scene: return "Scene";
        case AssetType::Prefab: return "Prefab";
        case AssetType::Shader: return "Shader";
        case AssetType::Unknown:
        case AssetType::Count: break;
    }
    return "Unknown";
}

[[nodiscard]] AssetType AssetTypeFromString(std::string_view text) noexcept;

/// Maps a file extension (with or without the dot) to an asset type.
[[nodiscard]] AssetType AssetTypeFromExtension(std::string_view extension) noexcept;

/// Case insensitive path equality, exposed so AssetPath can compare inline.
[[nodiscard]] bool PathsEqual(std::string_view a, std::string_view b) noexcept;

/// A normalised, root relative, case insensitive path.
///
/// Value type.  An empty text means "invalid" - there is no valid empty path.
class AssetPath {
public:
    AssetPath() = default;
    /// Lenient: normalises what it can and leaves the path invalid when the
    /// input tries to escape the root.  Use Create() to get an error instead.
    explicit AssetPath(std::string_view raw) { Assign(raw); }

    [[nodiscard]] static Result<AssetPath> Create(std::string_view raw);

    void Assign(std::string_view raw);

    [[nodiscard]] std::string_view Text() const noexcept { return text_; }
    [[nodiscard]] bool IsValid() const noexcept { return !text_.empty(); }
    [[nodiscard]] explicit operator bool() const noexcept { return IsValid(); }
    [[nodiscard]] std::string ToString() const { return text_; }

    /// Directory part, or empty for a top level file.
    [[nodiscard]] AssetPath Parent() const;
    /// File name including extension.
    [[nodiscard]] std::string_view FileName() const noexcept;
    /// File name without extension.
    [[nodiscard]] std::string_view Stem() const noexcept;
    /// Lowercased extension including the dot; empty when there is none.
    [[nodiscard]] std::string Extension() const;
    [[nodiscard]] AssetType Type() const noexcept { return AssetTypeFromExtension(Extension()); }

    [[nodiscard]] AssetPath WithExtension(std::string_view extension) const;
    [[nodiscard]] AssetPath Child(std::string_view name) const;
    /// Appends a suffix without touching the extension: "cube.glb" + ".l3dmeta"
    /// becomes "cube.glb.l3dmeta", which is how sidecar files are named.
    [[nodiscard]] AssetPath Appending(std::string_view suffix) const;

    [[nodiscard]] u64 Hash() const noexcept { return HashStringCaseInsensitive(text_); }

    friend bool operator==(const AssetPath& a, const AssetPath& b) noexcept {
        return PathsEqual(a.text_, b.text_);
    }
    friend bool operator!=(const AssetPath& a, const AssetPath& b) noexcept { return !(a == b); }
    /// Case sensitive ordering, so iteration is deterministic even for paths
    /// that only differ in case (which the database reports as a conflict).
    friend auto operator<=>(const AssetPath& a, const AssetPath& b) noexcept {
        return a.text_ <=> b.text_;
    }

private:
    std::string text_;
};

/// Meta files live next to their source: "models/cube.glb.l3dmeta".
inline constexpr std::string_view kMetaExtension = ".l3dmeta";

} // namespace l3d::assets

namespace std {
template <>
struct hash<l3d::assets::AssetId> {
    [[nodiscard]] std::size_t operator()(const l3d::assets::AssetId& id) const noexcept {
        return static_cast<std::size_t>(id.Hash());
    }
};
template <>
struct hash<l3d::assets::AssetPath> {
    [[nodiscard]] std::size_t operator()(const l3d::assets::AssetPath& path) const noexcept {
        return static_cast<std::size_t>(path.Hash());
    }
};
} // namespace std
