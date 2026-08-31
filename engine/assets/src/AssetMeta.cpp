#include "local3d/assets/AssetMeta.hpp"

#include "local3d/core/Hash.hpp"

#include <array>
#include <charconv>

namespace l3d::assets {

namespace {

/// Hashes are stored as hex strings: a JSON number is a double and would lose
/// the top bits of a 64 bit hash.
[[nodiscard]] std::string ToHex(u64 value) {
    std::array<char, 17> buffer{};
    const auto [end, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    if (ec != std::errc{}) {
        return "0";
    }
    return std::string(buffer.data(), end);
}

[[nodiscard]] u64 FromHex(std::string_view text, u64 fallback = 0) {
    u64 value = fallback;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, 16);
    if (ec != std::errc{} || ptr != last) {
        return fallback;
    }
    return value;
}

[[nodiscard]] std::string_view HexString(const serial::JsonValue& value) {
    return value.AsString();
}

} // namespace

u64 HashSettings(const serial::JsonValue& settings) {
    return HashString(settings.Dump(0));
}

u64 ImportFingerprintOf(std::string_view importer, u32 importerVersion,
                        u64 settingsHash) noexcept {
    return HashOf(HashString(importer), static_cast<u64>(importerVersion), settingsHash);
}

u64 AssetMeta::ImportFingerprint() const {
    return ImportFingerprintOf(importer, importerVersion, HashSettings(settings));
}

std::string AssetMeta::Dump() const {
    serial::JsonValue root = serial::JsonValue::MakeObject();
    root.Set("formatVersion", static_cast<u32>(formatVersion));
    root.Set("id", id.ToString());
    root.Set("type", std::string(AssetTypeToString(type)));
    root.Set("importer", importer);
    root.Set("importerVersion", importerVersion);
    root.Set("settings", settings);
    root.Set("cookedSourceHash", ToHex(cookedSourceHash));
    root.Set("cookedImportHash", ToHex(cookedImportHash));
    return root.Dump(2) + "\n";
}

Result<AssetMeta> AssetMeta::Parse(std::string_view jsonText) {
    auto parsed = serial::JsonValue::Parse(jsonText);
    if (parsed.IsError()) {
        return Unexpected(parsed.Error());
    }
    const serial::JsonValue& root = *parsed;
    if (!root.IsObject()) {
        return Unexpected(Status{StatusCode::ParseError, "Meta file must be a JSON object"});
    }

    AssetMeta meta;
    meta.formatVersion = static_cast<u32>(root["formatVersion"].AsInt(kCurrentFormatVersion));
    if (meta.formatVersion == 0 || meta.formatVersion > kCurrentFormatVersion) {
        return Unexpected(Status{StatusCode::Unsupported, "Meta file format version is not supported"});
    }

    auto id = AssetId::Parse(root["id"].AsString());
    if (id.IsError()) {
        return Unexpected(id.Error());
    }
    meta.id = *id;
    meta.type = AssetTypeFromString(root["type"].AsString("Unknown"));
    meta.importer = std::string(root["importer"].AsString());
    meta.importerVersion = static_cast<u32>(root["importerVersion"].AsInt(0));
    if (root["settings"].IsObject()) {
        meta.settings = root["settings"];
    }
    meta.cookedSourceHash = FromHex(HexString(root["cookedSourceHash"]));
    meta.cookedImportHash = FromHex(HexString(root["cookedImportHash"]));
    return meta;
}

Result<AssetMeta> AssetMeta::Load(IFileSystem& fs, const AssetPath& sourcePath) {
    const AssetPath metaPath = MetaPathFor(sourcePath);
    auto text = ReadFileAsText(fs, metaPath.Text());
    if (text.IsError()) {
        return Unexpected(text.Error());
    }
    return Parse(*text);
}

OperationResult AssetMeta::Save(IFileSystem& fs, const AssetPath& sourcePath) const {
    const AssetPath metaPath = MetaPathFor(sourcePath);
    const std::string text = Dump();
    return fs.WriteFile(metaPath.Text(),
                        std::as_bytes(std::span(text.data(), text.size())));
}

TextureImportSettings TextureSettingsFromJson(const serial::JsonValue& settings) noexcept {
    TextureImportSettings result;
    result.srgb = settings["srgb"].AsBool(true);
    result.generateMips = settings["generateMips"].AsBool(true);
    result.maxSize = static_cast<u32>(settings["maxSize"].AsInt(0));
    result.alphaIsCoverage = settings["alphaIsCoverage"].AsBool(false);
    return result;
}

serial::JsonValue TextureSettingsToJson(const TextureImportSettings& settings) {
    serial::JsonValue json = serial::JsonValue::MakeObject();
    json.Set("srgb", settings.srgb);
    json.Set("generateMips", settings.generateMips);
    json.Set("maxSize", settings.maxSize);
    json.Set("alphaIsCoverage", settings.alphaIsCoverage);
    return json;
}

} // namespace l3d::assets
