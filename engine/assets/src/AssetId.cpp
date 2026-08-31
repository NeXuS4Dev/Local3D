#include "local3d/assets/AssetId.hpp"

#include "local3d/assets/FileSystem.hpp"

#include <algorithm>
#include <array>

namespace l3d::assets {

namespace {

[[nodiscard]] char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string LowerAscii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](char c) { return LowerAscii(c); });
    return result;
}

/// Offset of the extension dot in a file name, or npos when there is none.
/// A leading dot is part of the name (".gitignore" has no extension).
[[nodiscard]] usize ExtensionDot(std::string_view fileName) noexcept {
    const usize dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return std::string_view::npos;
    }
    return dot;
}

} // namespace

Result<AssetId> AssetId::Parse(std::string_view text) noexcept {
    Uuid parsed;
    if (!Uuid::Parse(text, parsed)) {
        return Unexpected(Status{StatusCode::ParseError, "Malformed asset id"});
    }
    return AssetId{parsed};
}

std::string to_string(const AssetId& id) {
    return id.ToString();
}

AssetType AssetTypeFromString(std::string_view text) noexcept {
    for (u32 i = 0; i < static_cast<u32>(AssetType::Count); ++i) {
        const auto type = static_cast<AssetType>(i);
        if (AssetTypeToString(type) == text) {
            return type;
        }
    }
    return AssetType::Unknown;
}

AssetType AssetTypeFromExtension(std::string_view extension) noexcept {
    std::string ext = LowerAscii(extension);
    if (!ext.empty() && ext.front() != '.') {
        ext.insert(ext.begin(), '.');
    }
    static constexpr std::array<std::pair<std::string_view, AssetType>, 15> kExtensions{{
        {".glb", AssetType::Mesh},
        {".gltf", AssetType::Mesh},
        {".obj", AssetType::Mesh},
        {".fbx", AssetType::Mesh},
        {".png", AssetType::Texture},
        {".jpg", AssetType::Texture},
        {".jpeg", AssetType::Texture},
        {".tga", AssetType::Texture},
        {".bmp", AssetType::Texture},
        {".hdr", AssetType::Texture},
        {".wav", AssetType::AudioClip},
        {".l3dmat", AssetType::Material},
        {".l3dscene", AssetType::Scene},
        {".l3dprefab", AssetType::Prefab},
        {".spv", AssetType::Shader},
    }};
    for (const auto& [candidate, type] : kExtensions) {
        if (candidate == ext) {
            return type;
        }
    }
    return AssetType::Unknown;
}

bool PathsEqual(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

// --- AssetPath -------------------------------------------------------------

Result<AssetPath> AssetPath::Create(std::string_view raw) {
    AssetPath path;
    path.Assign(raw);
    if (!path.IsValid()) {
        return Unexpected(
            Status{StatusCode::InvalidArgument, "Asset path must stay inside the root"});
    }
    return path;
}

void AssetPath::Assign(std::string_view raw) {
    if (!NormalizePath(raw, text_)) {
        text_.clear();
    }
}

AssetPath AssetPath::Parent() const {
    const usize slash = text_.rfind('/');
    if (slash == std::string_view::npos) {
        return AssetPath{};
    }
    return AssetPath{std::string_view(text_).substr(0, slash)};
}

std::string_view AssetPath::FileName() const noexcept {
    const usize slash = text_.rfind('/');
    return std::string_view(text_).substr(slash == std::string_view::npos ? 0 : slash + 1);
}

std::string_view AssetPath::Stem() const noexcept {
    const std::string_view fileName = FileName();
    const usize dot = ExtensionDot(fileName);
    return dot == std::string_view::npos ? fileName : fileName.substr(0, dot);
}

std::string AssetPath::Extension() const {
    const std::string_view fileName = FileName();
    const usize dot = ExtensionDot(fileName);
    return dot == std::string_view::npos ? std::string{} : LowerAscii(fileName.substr(dot));
}

AssetPath AssetPath::WithExtension(std::string_view extension) const {
    const usize slash = text_.rfind('/');
    const usize nameStart = slash == std::string::npos ? 0 : slash + 1;
    const std::string_view fileName(text_.data() + nameStart, text_.size() - nameStart);
    const usize dot = ExtensionDot(fileName);
    const usize nameEnd = nameStart + (dot == std::string_view::npos ? fileName.size() : dot);

    std::string result = text_.substr(0, nameEnd);
    if (!extension.empty() && extension.front() != '.') {
        result += '.';
    }
    result += LowerAscii(extension);
    return AssetPath{result};
}

AssetPath AssetPath::Child(std::string_view name) const {
    std::string normalized;
    if (!NormalizePath(name, normalized)) {
        return AssetPath{};
    }
    if (text_.empty()) {
        return AssetPath{normalized};
    }
    return AssetPath{text_ + "/" + normalized};
}

AssetPath AssetPath::Appending(std::string_view suffix) const {
    if (text_.empty()) {
        return AssetPath{};
    }
    return AssetPath{text_ + std::string(suffix)};
}

} // namespace l3d::assets
